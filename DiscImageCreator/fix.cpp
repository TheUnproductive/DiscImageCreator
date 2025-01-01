/**
 * Copyright 2011-2025 sarami
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "struct.h"
#include "check.h"
#include "convert.h"
#include "get.h"
#include "output.h"
#include "outputScsiCmdLogforCD.h"
#include "set.h"
#include "calcHash.h"

 // This global variable is set if function is error
LONG s_lineNum;

VOID FixMainHeader(
	PEXT_ARG pExtArg,
	PDISC pDisc,
	PDISC_PER_SECTOR pDiscPerSector,
	INT nLBA,
	INT nMainDataType
) {
	BYTE lpWorkBuf[CD_RAW_SECTOR_SIZE] = {};
	if (nMainDataType == scrambled) {
		size_t ofs = CD_RAW_SECTOR_SIZE - pDisc->MAIN.uiMainDataSlideSize;
		memcpy(lpWorkBuf, pDiscPerSector->data.current + pDisc->MAIN.uiMainDataSlideSize, ofs);
		if (pDiscPerSector->data.next) {
			memcpy(lpWorkBuf + ofs, pDiscPerSector->data.next, pDisc->MAIN.uiMainDataSlideSize);
		}
	}
	else {
		memcpy(lpWorkBuf, pDiscPerSector->data.current, CD_RAW_SECTOR_SIZE);
	}

	BOOL bChangedBuf = FALSE;
	if (pDisc->PROTECT.byExist == datel || pDisc->PROTECT.byExist == datelAlt) {
		if (lpWorkBuf[15] != 0x62) {
			OutputMainErrorWithLBALog("Original Mode[0x%02x] -> Fixed Mode[0x%02x]\n"
				, nLBA, pDiscPerSector->byTrackNum, lpWorkBuf[15], pDiscPerSector->mainHeader.current[15]);
			lpWorkBuf[15] = pDiscPerSector->mainHeader.current[15];
			bChangedBuf = TRUE;
		}
	}
	BOOL bHeader = IsValidMainDataHeader(lpWorkBuf);
	if (!bHeader) {
		INT idx = pDiscPerSector->byTrackNum - 1;
		if (pDisc->PROTECT.byExist == datel || pDisc->PROTECT.byExist == datelAlt || pDisc->PROTECT.byExist == codelock) {
			if (((pDisc->SCSI.toc.TrackData[idx].Control & AUDIO_DATA_TRACK) == AUDIO_DATA_TRACK) &&
				((pDiscPerSector->subch.current.byCtl & AUDIO_DATA_TRACK) == AUDIO_DATA_TRACK)) {
				if (IsValidProtectedSector(pDisc, nLBA, GetReadErrorFileIdx(pExtArg, pDisc, nLBA))) {
					OutputMainErrorWithLBALog(
						"This sector is data, but sync is invalid\n", nLBA, pDiscPerSector->byTrackNum);
					OutputMainChannel(fileMainError, lpWorkBuf, NULL, nLBA, MAINHEADER_MODE1_SIZE);
					memcpy(lpWorkBuf, pDiscPerSector->mainHeader.current, MAINHEADER_MODE1_SIZE);

					OutputMainErrorWithLBALog(
						"Sync was generated\n", nLBA, pDiscPerSector->byTrackNum);
					OutputMainChannel(fileMainError, lpWorkBuf, NULL, nLBA, MAINHEADER_MODE1_SIZE);
					bHeader = TRUE;
					bChangedBuf = TRUE;
					FlushLog();
				}
			}
		}
	}

	if (bChangedBuf) {
		if (nMainDataType == scrambled) {
			size_t ofs = CD_RAW_SECTOR_SIZE - pDisc->MAIN.uiMainDataSlideSize;
			memcpy(pDiscPerSector->data.current + pDisc->MAIN.uiMainDataSlideSize, lpWorkBuf, ofs);
			if (pDiscPerSector->data.next) {
				memcpy(pDiscPerSector->data.next, lpWorkBuf + ofs, pDisc->MAIN.uiMainDataSlideSize);
			}
		}
		else {
			memcpy(pDiscPerSector->data.current, lpWorkBuf, CD_RAW_SECTOR_SIZE);
		}
	}
	memcpy(pDiscPerSector->mainHeader.current, lpWorkBuf, MAINHEADER_MODE1_SIZE);
	UpdateTmpMainHeader(pDiscPerSector, nMainDataType);
}

VOID FixSubP(
	PDISC_PER_SECTOR pDiscPerSector,
	INT nLBA
) {
	for (INT i = 0; i < 12; i++) {
		if (pDiscPerSector->subch.current.byP == 0xff &&
			pDiscPerSector->subcode.current[i] != 0xff) {
			OutputSubErrorWithLBALog("P[%02d]:[%#04x] -> [0xff]\n"
				, nLBA, pDiscPerSector->byTrackNum, i, pDiscPerSector->subcode.current[i]);
			pDiscPerSector->subcode.current[i] = 0xff;
		}
		else if (pDiscPerSector->subch.current.byP == 0x00 &&
			pDiscPerSector->subcode.current[i] != 0x00) {
			OutputSubErrorWithLBALog("P[%02d]:[%#04x] -> [0x00]\n"
				, nLBA, pDiscPerSector->byTrackNum, i, pDiscPerSector->subcode.current[i]);
			pDiscPerSector->subcode.current[i] = 0x00;
		}
	}
	return;
}

BOOL FixSubQAdrMCN(
	PEXEC_TYPE pExecType,
	PEXT_ARG pExtArg,
	PDISC pDisc,
	PDISC_PER_SECTOR pDiscPerSector,
	INT nLBA
) {
	if (!pDisc->SUB.byCatalog) {
		return FALSE;
	}
	else if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_MEDIA_CATALOG) {
		CHAR szCatalog[META_CATALOG_SIZE] = {};
		BOOL bMCN = IsValidSubQAdrMCN(pDiscPerSector->subcode.current);
		SetMCNToString(pDisc, pDiscPerSector->subcode.current, szCatalog, FALSE);

		if (!strncmp(pDisc->SUB.szCatalog, szCatalog, META_CATALOG_SIZE) && bMCN) {
			return TRUE;
		}
	}
	BOOL bRet = TRUE;
	INT session = 1;
	if (*pExecType != swap && pDiscPerSector->byTrackNum > 0) {
		session = pDisc->SCSI.lpSessionNumList[pDiscPerSector->byTrackNum - 1];
		INT nRangeLBA = pDisc->SUB.nRangeLBAForMCN[0][session - 1];
		if (nRangeLBA == -1) {
			return FALSE;
		}

		INT n1stLBA = pDisc->SUB.n1stLBAForMCN[0][session - 1];
		INT nPrevMCNSector = pDisc->SUB.nPrevMCNSector;
		bRet = IsValidSubQAdrSector(pExtArg->uiSubAddionalNum, &pDiscPerSector->subch,
			nRangeLBA, n1stLBA, nPrevMCNSector, nLBA);
		if (!bRet) {
			nRangeLBA = pDisc->SUB.nRangeLBAForMCN[1][session - 1];
			if (nRangeLBA != -1) {
				n1stLBA = pDisc->SUB.n1stLBAForMCN[1][session - 1];
				bRet = IsValidSubQAdrSector(pExtArg->uiSubAddionalNum, &pDiscPerSector->subch, nRangeLBA, n1stLBA, nPrevMCNSector, nLBA);
			}
		}
	}
	if (bRet) {
		if (pDiscPerSector->subch.current.byAdr != ADR_ENCODES_MEDIA_CATALOG &&
			pDiscPerSector->subch.prev.byAdr != ADR_ENCODES_MEDIA_CATALOG) {
			OutputSubErrorWithLBALog("Q[12]:Adr[%d] -> [0x02]\n"
				, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.current.byAdr);
			pDiscPerSector->subch.current.byAdr = ADR_ENCODES_MEDIA_CATALOG;
			pDiscPerSector->subcode.current[12] =
				(BYTE)(pDiscPerSector->subch.current.byCtl << 4 | pDiscPerSector->subch.current.byAdr);
		}
		if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_MEDIA_CATALOG) {
			CHAR szCatalog[META_CATALOG_SIZE] = {};
			BOOL bMCN = IsValidSubQAdrMCN(pDiscPerSector->subcode.current);
			SetMCNToString(pDisc, pDiscPerSector->subcode.current, szCatalog, FALSE);

			if (strncmp(pDisc->SUB.szCatalog, szCatalog, META_CATALOG_SIZE) || !bMCN) {
				OutputSubErrorWithLBALog(
					"Q[13-19]:MCN[%13" CHARWIDTH "s], Sub[19]Lo:[%x], Sub[20]:[%#04x] -> [%13" CHARWIDTH "s], [19]Lo:[0], [20]:[0x00]\n"
					, nLBA, pDiscPerSector->byTrackNum, szCatalog, pDiscPerSector->subcode.current[19] & 0x0f
					, pDiscPerSector->subcode.current[20], pDisc->SUB.szCatalog);
				SetBufferFromMCN(pDisc, pDiscPerSector->subcode.current);
			}
			bRet = TRUE;
		}
	}
	else {
		if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_MEDIA_CATALOG) {
			CHAR szCatalog[META_CATALOG_SIZE] = {};
			BOOL bMCN = IsValidSubQAdrMCN(pDiscPerSector->subcode.current);
			SetMCNToString(pDisc, pDiscPerSector->subcode.current, szCatalog, FALSE);

			if (strncmp(pDisc->SUB.szCatalog, szCatalog, META_CATALOG_SIZE) || !bMCN) {
				OutputSubErrorWithLBALog("Q[12]:Adr[%d] -> No MCN frame\n"
					, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.current.byAdr);
				return FALSE;
			}
			INT nTmpFirstLBA = nLBA % pDisc->SUB.nRangeLBAForMCN[0][session - 1];
			OutputMainInfoWithLBALog("Range of MCN is different [%d]\n"
				, nLBA, pDiscPerSector->byTrackNum, nLBA - pDisc->SUB.nPrevMCNSector);
			pDisc->SUB.n1stLBAForMCN[0][session - 1] = nTmpFirstLBA;
			bRet = TRUE;
		}
	}

	if (bRet) {
		pDisc->SUB.nPrevMCNSector = nLBA;
		if ((pDiscPerSector->subcode.current[19] & 0x0f) != 0) {
			OutputSubErrorWithLBALog("Q[19]:[%x] -> [%x]\n", nLBA, pDiscPerSector->byTrackNum
				, pDiscPerSector->subcode.current[19], pDiscPerSector->subcode.current[19] & 0xf0);
			pDiscPerSector->subcode.current[19] &= 0xf0;
		}
		if (pDiscPerSector->subcode.current[20] != 0) {
			OutputSubErrorWithLBALog("Q[20]:[%x] -> [0x00]\n"
				, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subcode.current[20]);
			pDiscPerSector->subcode.current[20] = 0;
		}
		UpdateTmpSubchForMCN(pDisc, pDiscPerSector, nLBA);
	}
	return bRet;
}

BOOL FixSubQAdrISRC(
	PEXEC_TYPE pExecType,
	PEXT_ARG pExtArg,
	PDISC pDisc,
	PDISC_PER_SECTOR pDiscPerSector,
	INT nLBA
) {
	if (pDiscPerSector->byTrackNum > 0 && pDisc->SUB.lpISRCList[pDiscPerSector->byTrackNum - 1] == 0) {
		return FALSE;
	}
	else if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_ISRC) {
		CHAR szISRC[META_ISRC_SIZE] = {};
		BOOL bISRC = IsValidSubQAdrISRC(pDiscPerSector->subcode.current);
		SetISRCToString(pDisc, pDiscPerSector, szISRC, FALSE);

		if (pDiscPerSector->byTrackNum > 0 && !strncmp(pDisc->SUB.pszISRC[pDiscPerSector->byTrackNum - 1], szISRC, META_ISRC_SIZE) && bISRC) {
			return TRUE;
		}
	}
	BOOL bRet = TRUE;
	INT session = 1;
	if (*pExecType != swap && pDiscPerSector->byTrackNum > 0) {
		session = pDisc->SCSI.lpSessionNumList[pDiscPerSector->byTrackNum - 1];
		INT nRangeLBA = pDisc->SUB.nRangeLBAForISRC[0][session - 1];
		if (nRangeLBA == -1) {
			return FALSE;
		}
		INT n1stLBA = pDisc->SUB.n1stLBAForISRC[0][session - 1];
		INT nPrevISRCSector = pDisc->SUB.nPrevISRCSector;
		bRet = IsValidSubQAdrSector(pExtArg->uiSubAddionalNum, &pDiscPerSector->subch, nRangeLBA, n1stLBA, nPrevISRCSector, nLBA);
		if (!bRet) {
			nRangeLBA = pDisc->SUB.nRangeLBAForISRC[1][session - 1];
			if (nRangeLBA != -1) {
				n1stLBA = pDisc->SUB.n1stLBAForISRC[1][session - 1];
				bRet = IsValidSubQAdrSector(pExtArg->uiSubAddionalNum, &pDiscPerSector->subch, nRangeLBA, n1stLBA, nPrevISRCSector, nLBA);
			}
		}
	}
	if (bRet) {
		if (pDiscPerSector->subch.current.byAdr != ADR_ENCODES_ISRC &&
			pDiscPerSector->subch.prev.byAdr != ADR_ENCODES_ISRC) {
			OutputSubErrorWithLBALog("Q[12]:Adr[%d] -> [0x03]\n"
				, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.current.byAdr);
			pDiscPerSector->subch.current.byAdr = ADR_ENCODES_ISRC;
			pDiscPerSector->subcode.current[12] =
				(BYTE)(pDiscPerSector->subch.current.byCtl << 4 | pDiscPerSector->subch.current.byAdr);
		}
		if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_ISRC) {
			CHAR szISRC[META_ISRC_SIZE] = {};
			BOOL bISRC = IsValidSubQAdrISRC(pDiscPerSector->subcode.current);
			SetISRCToString(pDisc, pDiscPerSector, szISRC, FALSE);

			if ((pDiscPerSector->byTrackNum > 0 && strncmp(pDisc->SUB.pszISRC[pDiscPerSector->byTrackNum - 1], szISRC, META_ISRC_SIZE)) || !bISRC) {
				OutputSubErrorWithLBALog(
					"Q[13-20]:ISRC[%12" CHARWIDTH "s], SubQ[20]Lo:[%x] -> [%12" CHARWIDTH "s], SubQ[20]Lo:[0]\n"
					, nLBA, pDiscPerSector->byTrackNum, szISRC
					, pDiscPerSector->subcode.current[20] & 0x0f, pDisc->SUB.pszISRC[pDiscPerSector->byTrackNum - 1]);

				CHAR tmpISRC[META_ISRC_SIZE] = {};
				strncpy(tmpISRC, pDisc->SUB.pszISRC[pDiscPerSector->byTrackNum - 1], SIZE_OF_ARRAY(tmpISRC) - 1);
				pDiscPerSector->subcode.current[13] = (BYTE)(((tmpISRC[0] - 0x30) << 2) | ((tmpISRC[1] - 0x30) >> 4));
				pDiscPerSector->subcode.current[14] = (BYTE)(((tmpISRC[1] - 0x30) << 4) | ((tmpISRC[2] - 0x30) >> 2));
				pDiscPerSector->subcode.current[15] = (BYTE)(((tmpISRC[2] - 0x30) << 6) | (tmpISRC[3] - 0x30));
				pDiscPerSector->subcode.current[16] = (BYTE)((tmpISRC[4] - 0x30) << 2);
				for (INT i = 17, j = 5; i < 20; i++, j += 2) {
					pDiscPerSector->subcode.current[i] = (BYTE)(tmpISRC[j] - 0x30);
					pDiscPerSector->subcode.current[i] = (BYTE)(pDiscPerSector->subcode.current[i] << 4);
					pDiscPerSector->subcode.current[i] |= (BYTE)(tmpISRC[j + 1] - 0x30);
				}
				pDiscPerSector->subcode.current[20] = (BYTE)(tmpISRC[11] - 0x30);
				pDiscPerSector->subcode.current[20] = (BYTE)(pDiscPerSector->subcode.current[20] << 4);
			}
			bRet = TRUE;
		}
	}
	else {
		if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_ISRC) {
			CHAR szISRC[META_ISRC_SIZE] = {};
			BOOL bISRC = IsValidSubQAdrISRC(pDiscPerSector->subcode.current);
			SetISRCToString(pDisc, pDiscPerSector, szISRC, FALSE);

			if ((pDiscPerSector->byTrackNum > 0 && strncmp(pDisc->SUB.pszISRC[pDiscPerSector->byTrackNum - 1], szISRC, META_ISRC_SIZE)) || !bISRC) {
				OutputSubErrorWithLBALog("Q[12]:Adr[%d] -> No ISRC frame\n"
					, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.current.byAdr);
				return FALSE;
			}
			INT nTmpFirstLBA = nLBA % pDisc->SUB.nRangeLBAForISRC[0][session - 1];
			OutputMainInfoWithLBALog("Range of ISRC is different [%d]\n"
				, nLBA, pDiscPerSector->byTrackNum, nLBA - pDisc->SUB.nPrevISRCSector);
			pDisc->SUB.n1stLBAForISRC[0][session - 1] = nTmpFirstLBA;
			bRet = TRUE;
		}
	}

	if (bRet) {
		pDisc->SUB.nPrevISRCSector = nLBA;
		// because tracknum, index... doesn't exist
		if (pDiscPerSector->subch.current.byAdr != ADR_ENCODES_ISRC) {
			pDiscPerSector->subch.current.byAdr = ADR_ENCODES_ISRC;
			pDiscPerSector->subcode.current[12] =
				(BYTE)(pDiscPerSector->subch.current.byCtl << 4 | pDiscPerSector->subch.current.byAdr);
		}
		if ((pDiscPerSector->subcode.current[16] & 0x03) != 0) {
			OutputSubErrorWithLBALog("Q[16]:[%x] -> [%x]\n", nLBA, pDiscPerSector->byTrackNum
				, pDiscPerSector->subcode.current[16], pDiscPerSector->subcode.current[16] & 0xfc);
			pDiscPerSector->subcode.current[16] &= 0xfc;
		}
		if ((pDiscPerSector->subcode.current[20] & 0x0f) != 0) {
			OutputSubErrorWithLBALog("Q[20]:[%x] -> [%x]\n", nLBA, pDiscPerSector->byTrackNum
				, pDiscPerSector->subcode.current[20], pDiscPerSector->subcode.current[20] & 0xf0);
			pDiscPerSector->subcode.current[20] &= 0xf0;
		}
		UpdateTmpSubchForISRC(&pDiscPerSector->subch);
	}
	return bRet;
}

BOOL FixSubQAdr6(
	PDISC pDisc,
	PDISC_PER_SECTOR pDiscPerSector,
	INT nLBA
) {
	if (!pDisc->SUB.byAdr6) {
		return FALSE;
	}
	else if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_6) {
		CHAR szAdr6[META_ADR6_SIZE] = {};
		SetAdr6ToString(pDisc, pDiscPerSector->subcode.current, szAdr6, FALSE);

		if (!strncmp(pDisc->SUB.szAdr6, szAdr6, META_ADR6_SIZE)) {
			return TRUE;
		}
	}

	if (pDiscPerSector->subch.current.byAdr != ADR_ENCODES_6 &&
		pDiscPerSector->subch.prev.byAdr != ADR_ENCODES_6) {
		OutputSubErrorWithLBALog("Q[12]:Adr[%d] -> [0x06]\n"
			, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.current.byAdr);
		pDiscPerSector->subch.current.byAdr = ADR_ENCODES_6;
		pDiscPerSector->subcode.current[12] =
			(BYTE)(pDiscPerSector->subch.current.byCtl << 4 | pDiscPerSector->subch.current.byAdr);
	}
	if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_6) {
		CHAR szAdr6[META_ADR6_SIZE] = {};
		SetAdr6ToString(pDisc, pDiscPerSector->subcode.current, szAdr6, FALSE);

		if (strncmp(pDisc->SUB.szAdr6, szAdr6, META_ADR6_SIZE)) {
			OutputSubErrorWithLBALog(
				"Q[13-19]:Adr6[%16" CHARWIDTH "s] -> [%16" CHARWIDTH "s]\n"
				, nLBA, pDiscPerSector->byTrackNum, szAdr6, pDisc->SUB.szAdr6);
			sscanf(pDisc->SUB.szAdr6, "%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx%2hhx"
				, &pDiscPerSector->subcode.current[13], &pDiscPerSector->subcode.current[14]
				, &pDiscPerSector->subcode.current[15], &pDiscPerSector->subcode.current[16]
				, &pDiscPerSector->subcode.current[17], &pDiscPerSector->subcode.current[18]
				, &pDiscPerSector->subcode.current[19], &pDiscPerSector->subcode.current[20]
			);
		}
	}
	return TRUE;
}

WORD RecalcSubQCrc(
	PDISC pDisc,
	PDISC_PER_SECTOR pDiscPerSector
) {
	WORD crc16 = (WORD)GetCrc16CCITT(10, &pDiscPerSector->subcode.current[12]);
	pDisc->SUB.nCorruptCrcH = pDiscPerSector->subcode.current[22] == HIBYTE(crc16) ? FALSE : TRUE;
	pDisc->SUB.nCorruptCrcL = pDiscPerSector->subcode.current[23] == LOBYTE(crc16) ? FALSE : TRUE;
	return crc16;
}

VOID FixSubQ(
	PEXEC_TYPE pExecType,
	PEXT_ARG pExtArg,
	PDISC pDisc,
	PDISC_PER_SECTOR pDiscPerSector,
	INT nLBA
) {
	if (pDisc->PROTECT.byExist == datel || pDisc->PROTECT.byExist == datelAlt) {
		//========== LBA[145406, 0x237fe]: Sub Channel ==========
		//	  +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B
		//	P FF FF FF FF FF FF FF FF FF 00 00 00
		//	Q 83 54 02 4C 80 B2 00 64 40 41 AA 01
		//========== LBA[145407, 0x237ff]: Sub Channel ==========
		//	  +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B
		//	P 00 00 00 00 00 00 00 00 00 00 00 00
		//	Q 26 40 60 00 32 20 56 79 AA 41 AA 01
		if (pDiscPerSector->subcode.current[21] == 0x41 &&
			(pDiscPerSector->subcode.current[22] == 0xaa || pDiscPerSector->subcode.current[22] == 0x01) &&
			pDiscPerSector->subcode.current[23] == 0x01) {
			pDiscPerSector->subcode.current[12] = pDiscPerSector->subcode.current[21];
			pDiscPerSector->subcode.current[13] = pDiscPerSector->subcode.current[22];
			pDiscPerSector->subcode.current[14] = pDiscPerSector->subcode.current[23];
			pDiscPerSector->subcode.current[15] = pDiscPerSector->subcode.next[12];
			pDiscPerSector->subcode.current[16] = pDiscPerSector->subcode.next[13];
			pDiscPerSector->subcode.current[17] = pDiscPerSector->subcode.next[14];
			pDiscPerSector->subcode.current[18] = pDiscPerSector->subcode.next[15];
			pDiscPerSector->subcode.current[19] = pDiscPerSector->subcode.next[16];
			pDiscPerSector->subcode.current[20] = pDiscPerSector->subcode.next[17];
			pDiscPerSector->subcode.current[21] = pDiscPerSector->subcode.next[18];
			pDiscPerSector->subcode.current[22] = pDiscPerSector->subcode.next[19];
			pDiscPerSector->subcode.current[23] = pDiscPerSector->subcode.next[20];
			SetTmpSubchFromBuffer(&pDiscPerSector->subch.current, pDiscPerSector->subcode.current);
			OutputSubErrorWithLBALog("Q[12-23] all replaced\n", nLBA, pDiscPerSector->byTrackNum);
		}
	}
	else if (IsValidIntentionalC2error(pDisc, pDiscPerSector, nLBA, GetC2ErrorFileIdx(pExtArg, pDisc, nLBA))) {
		if (pDiscPerSector->bReturnCode == RETURNED_EXIST_C2_ERROR) {
			memcpy(pDiscPerSector->subcode.current, pDiscPerSector->subcode.prev, sizeof(pDiscPerSector->subcode.current));
			SetTmpSubchFromBuffer(&pDiscPerSector->subch.current, pDiscPerSector->subcode.current);
			OutputSubErrorWithLBALog("Q[12-23] all replaced\n", nLBA, pDiscPerSector->byTrackNum);
		}
	}

	if (pDiscPerSector->subch.prevPrev.byTrackNum == 110 ||
		pDiscPerSector->subch.prev.byTrackNum == 110 ||
		pDiscPerSector->subch.current.byTrackNum == 110) {
		// skip lead-out
		if (nLBA > pDisc->SCSI.nAllLength - 10) {
			return;
		}
		else if (pDiscPerSector->subcode.current[13] != 0xaa) {
			OutputSubErrorWithLBALog("Q[13]:TrackNum[%02x] -> [0xaa]\n"
				, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subcode.current[13]);
			pDiscPerSector->subcode.current[13] = 0xaa;
			pDiscPerSector->subch.current.byTrackNum = 110;

			if (pDiscPerSector->subch.current.byAdr >= ADR_ENCODES_MEDIA_CATALOG) {
				OutputSubErrorWithLBALog("Q[12]:Adr[%02x] -> [0x01]\n"
					, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subcode.current[13]);
				pDiscPerSector->subch.current.byAdr = ADR_ENCODES_CURRENT_POSITION;
			}

			RecalcSubQCrc(pDisc, pDiscPerSector);
			if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
				return;
			}
		}
	}

	BOOL bAdrCurrent = FALSE;
	if ((pDiscPerSector->subch.current.byAdr == ADR_ENCODES_MEDIA_CATALOG ||
		pDiscPerSector->subch.current.byAdr == ADR_ENCODES_ISRC ||
		pDiscPerSector->subch.current.byAdr == ADR_ENCODES_6) &&
		(pDiscPerSector->subch.prev.byAdr != ADR_ENCODES_MEDIA_CATALOG &&
		pDiscPerSector->subch.prev.byAdr != ADR_ENCODES_ISRC &&
		pDiscPerSector->subch.prev.byAdr != ADR_ENCODES_6)) {

		// assume that adr is incorrect
		BYTE byBackupAdr = (BYTE)(pDiscPerSector->subcode.current[12] & 0x0f);
		BYTE byBackupSub13 = pDiscPerSector->subcode.current[13];
		BYTE byBackupSub14 = pDiscPerSector->subcode.current[14];
		pDiscPerSector->subch.current.byAdr = ADR_ENCODES_CURRENT_POSITION;
		pDiscPerSector->subcode.current[12] =
			(BYTE)(pDiscPerSector->subch.current.byCtl << 4 | pDiscPerSector->subch.current.byAdr);
		if (pDiscPerSector->subch.prev.byTrackNum != 110) {
			if (pDiscPerSector->subcode.current[13] != DecToBcd(pDiscPerSector->subch.prev.byTrackNum)) {
				pDiscPerSector->subcode.current[13] = DecToBcd(pDiscPerSector->subch.prev.byTrackNum);
			}
		}
		if (pDiscPerSector->subcode.current[14] != DecToBcd(pDiscPerSector->subch.prev.byIndex)) {
			pDiscPerSector->subcode.current[14] = DecToBcd(pDiscPerSector->subch.prev.byIndex);
		}

		RecalcSubQCrc(pDisc, pDiscPerSector);
		if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
			OutputSubErrorWithLBALog("Q[12]:Adr[%d] -> [0x01]\n"
				, nLBA, pDiscPerSector->byTrackNum, byBackupAdr);
			return;
		}
		else if ((pDiscPerSector->subch.prev.byTrackNum == pDiscPerSector->subch.current.byTrackNum &&
			pDiscPerSector->subch.prev.byIndex == pDiscPerSector->subch.current.byIndex) ||
			(pDiscPerSector->subch.prevPrev.byTrackNum == pDiscPerSector->subch.current.byTrackNum &&
				pDiscPerSector->subch.prevPrev.byIndex == pDiscPerSector->subch.current.byIndex)) {
			OutputSubErrorWithLBALog("Q[12]:Adr[%d] -> [0x01]\n"
				, nLBA, pDiscPerSector->byTrackNum, byBackupAdr);
			pDiscPerSector->subch.current.byAdr = ADR_ENCODES_CURRENT_POSITION;
			pDiscPerSector->subcode.current[12] =
				(BYTE)(pDiscPerSector->subch.current.byCtl << 4 | pDiscPerSector->subch.current.byAdr);

			RecalcSubQCrc(pDisc, pDiscPerSector);
			if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
				return;
			}
		}
		else {
			pDiscPerSector->subch.current.byAdr = (BYTE)(byBackupAdr & 0x0f);
			pDiscPerSector->subcode.current[12] =
				(BYTE)(pDiscPerSector->subch.current.byCtl << 4 | pDiscPerSector->subch.current.byAdr);
			pDiscPerSector->subcode.current[13] = byBackupSub13;
			pDiscPerSector->subcode.current[14] = byBackupSub14;

			// assume that adr is correct but MCN or ISRC is incorrect
			if (1 <= pExtArg->uiSubAddionalNum) {
				// first check adr:2
				if (!FixSubQAdrMCN(pExecType, pExtArg, pDisc, pDiscPerSector, nLBA)) {
					// Next check adr:3
					if (!FixSubQAdrISRC(pExecType, pExtArg, pDisc, pDiscPerSector, nLBA)) {
						// Next check adr:6
						if (!FixSubQAdr6(pDisc, pDiscPerSector, nLBA)) {
							bAdrCurrent = TRUE;
						}
					}
				}
			}
			else {
				// If it doesn't read the next sector,
				// adr premises that it isn't ADR_ENCODES_CURRENT_POSITION
				if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_ISRC) {
					// first check adr:3
					if (!FixSubQAdrISRC(pExecType, pExtArg, pDisc, pDiscPerSector, nLBA)) {
						// Next check adr:2
						if (!FixSubQAdrMCN(pExecType, pExtArg, pDisc, pDiscPerSector, nLBA)) {
							// Next check adr:6
							if (!FixSubQAdr6(pDisc, pDiscPerSector, nLBA)) {
								bAdrCurrent = TRUE;
							}
						}
					}
				}
				else if (pDiscPerSector->subch.current.byAdr != ADR_ENCODES_CURRENT_POSITION) {
					// first check adr:2
					if (!FixSubQAdrMCN(pExecType, pExtArg, pDisc, pDiscPerSector, nLBA)) {
						// Next check adr:3
						if (!FixSubQAdrISRC(pExecType, pExtArg, pDisc, pDiscPerSector, nLBA)) {
							// Next check adr:6
							if (!FixSubQAdr6(pDisc, pDiscPerSector, nLBA)) {
								bAdrCurrent = TRUE;
							}
						}
					}
				}
			}
			if (bAdrCurrent) {
				if (pDiscPerSector->subch.current.byAdr > ADR_ENCODES_ISRC) {
					OutputSubErrorWithLBALog("Q[12]:Adr[%d] -> [0x01]\n"
						, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.current.byAdr);
					pDiscPerSector->subch.current.byAdr = ADR_ENCODES_CURRENT_POSITION;
					pDiscPerSector->subcode.current[12] =
						(BYTE)(pDiscPerSector->subch.current.byCtl << 4 | pDiscPerSector->subch.current.byAdr);

					RecalcSubQCrc(pDisc, pDiscPerSector);
					if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
						return;
					}
				}
			}
		}
	}
	// assume that adr is incorrect but MCN or ISRC is correct
	else {
		if (pDisc->SUB.byCatalog) {
			CHAR szCatalog[META_CATALOG_SIZE] = {};
			BOOL bMCN = IsValidSubQAdrMCN(pDiscPerSector->subcode.current);
			SetMCNToString(pDisc, pDiscPerSector->subcode.current, szCatalog, FALSE);

			if (!strncmp(pDisc->SUB.szCatalog, szCatalog, META_CATALOG_SIZE) && bMCN) {
				OutputSubErrorWithLBALog("Q[12]:Adr[%d] -> [0x02]\n"
					, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.current.byAdr);

				pDiscPerSector->subch.current.byAdr = ADR_ENCODES_MEDIA_CATALOG;
				pDiscPerSector->subcode.current[12] =
					(BYTE)(pDiscPerSector->subch.current.byCtl << 4 | pDiscPerSector->subch.current.byAdr);

				RecalcSubQCrc(pDisc, pDiscPerSector);
				if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
					return;
				}
			}
		}
		if (pDiscPerSector->byTrackNum > 0 && pDisc->SUB.lpISRCList[pDiscPerSector->byTrackNum - 1] == 0) {
			CHAR szISRC[META_ISRC_SIZE] = {};
			BOOL bISRC = IsValidSubQAdrISRC(pDiscPerSector->subcode.current);
			SetISRCToString(pDisc, pDiscPerSector, szISRC, FALSE);

			if (!strncmp(pDisc->SUB.pszISRC[pDiscPerSector->byTrackNum - 1], szISRC, META_ISRC_SIZE) && bISRC) {
				OutputSubErrorWithLBALog("Q[12]:Adr[%d] -> [0x03]\n"
					, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.current.byAdr);

				pDiscPerSector->subch.current.byAdr = ADR_ENCODES_ISRC;
				pDiscPerSector->subcode.current[12] =
					(BYTE)(pDiscPerSector->subch.current.byCtl << 4 | pDiscPerSector->subch.current.byAdr);

				RecalcSubQCrc(pDisc, pDiscPerSector);
				if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
					return;
				}
			}
		}
		if (pDisc->SUB.byAdr6) {
			CHAR szAdr6[META_ADR6_SIZE] = {};
			SetAdr6ToString(pDisc, pDiscPerSector->subcode.current, szAdr6, FALSE);

			if (!strncmp(pDisc->SUB.szAdr6, szAdr6, META_ADR6_SIZE)) {
				OutputSubErrorWithLBALog("Q[12]:Adr[%d] -> [0x06]\n"
					, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.current.byAdr);

				pDiscPerSector->subch.current.byAdr = ADR_ENCODES_6;
				pDiscPerSector->subcode.current[12] =
					(BYTE)(pDiscPerSector->subch.current.byCtl << 4 | pDiscPerSector->subch.current.byAdr);

				RecalcSubQCrc(pDisc, pDiscPerSector);
				if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
					return;
				}
			}
		}
		if (pDiscPerSector->subch.current.byAdr != ADR_ENCODES_CURRENT_POSITION &&
			pDiscPerSector->subch.current.byAdr != ADR_ENCODES_MEDIA_CATALOG &&
			pDiscPerSector->subch.current.byAdr != ADR_ENCODES_ISRC &&
			pDiscPerSector->subch.current.byAdr != ADR_ENCODES_6
			) {
			OutputSubErrorWithLBALog("Q[12]:Adr[%d] -> [0x01]\n"
				, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.current.byAdr);

			pDiscPerSector->subch.current.byAdr = ADR_ENCODES_CURRENT_POSITION;
			pDiscPerSector->subcode.current[12] =
				(BYTE)(pDiscPerSector->subch.current.byCtl << 4 | pDiscPerSector->subch.current.byAdr);

			RecalcSubQCrc(pDisc, pDiscPerSector);
			if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
				return;
			}
		}
	}

	BYTE SubQcodeOrg[12] = {};
	if (pDiscPerSector->bLibCrypt || pDiscPerSector->bSecuRom) {
		memcpy(SubQcodeOrg, &pDiscPerSector->subcode.current[12], sizeof(SubQcodeOrg));
	}

	if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_CURRENT_POSITION) {
		BOOL bPrevTrackNum = TRUE;
		if (!IsValidSubQTrack(pExecType, pDisc, pDiscPerSector, nLBA, &bPrevTrackNum)) {
			if (pDisc->PROTECT.byExist == datel && pDiscPerSector->subch.prev.byTrackNum == 110) {
				if (pDiscPerSector->subch.current.byTrackNum != 110) {
					OutputSubErrorWithLBALog("Q[13]:TrackNum[%02u] [L:%ld] -> 110\n"
						, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.current.byTrackNum, s_lineNum);
					pDiscPerSector->subch.current.byTrackNum = 110;
					pDiscPerSector->subcode.current[13] = 0xaa;

					RecalcSubQCrc(pDisc, pDiscPerSector);
					if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
						return;
					}
				}
			}
			else {
				OutputSubErrorWithLBALog("Q[13]:TrackNum[%02u] [L:%ld] -> "
					, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.current.byTrackNum, s_lineNum);
				BOOL bFix = FALSE;
				if (*pExecType != swap) {
					if (pDiscPerSector->byTrackNum == pDisc->SCSI.toc.LastTrack) {
						pDiscPerSector->subch.current.byTrackNum = pDisc->SCSI.toc.LastTrack;
						OutputSubErrorLog("[%02u], [L:%d]\n", pDisc->SCSI.toc.LastTrack, __LINE__);
						bFix = TRUE;
					}
					else if (pDisc->SCSI.lp1stLBAListOnToc[pDiscPerSector->byTrackNum] < nLBA) {
						pDiscPerSector->subch.current.byTrackNum = (BYTE)(pDiscPerSector->subch.prev.byTrackNum + 1);
						OutputSubErrorLog("[%02d], [L:%d]\n", pDiscPerSector->subch.prev.byTrackNum + 1, __LINE__);
						bFix = TRUE;
					}
					else if (pDiscPerSector->subch.current.byIndex == 0 && IsValidPregapSector(pDisc, &pDiscPerSector->subch, nLBA)) {
						// Network Q RAC Rally Championship (Netherlands)
						// LBA[202407, 0x316a7], Audio, 2ch, Copy NG, Pre-emphasis No, Track[13], Idx[01], RMSF[05:21:29], AMSF[45:00:57], RtoW[0, 0, 0, 0]
						// LBA[202408, 0x316a8], Audio, 2ch, Copy NG, Pre-emphasis No, Track[16], Idx[00], RMSF[00:01:74], AMSF[45:00:58], RtoW[0, 0, 0, 0]
						// LBA[202409, 0x316a9], Audio, 2ch, Copy NG, Pre-emphasis No, Track[14], Idx[00], RMSF[00:01:73], AMSF[45:00:59], RtoW[0, 0, 0, 0]
						pDiscPerSector->subch.current.byTrackNum = (BYTE)(pDiscPerSector->subch.prev.byTrackNum + 1);
						OutputSubErrorLog("[%02d], [L:%d]\n", pDiscPerSector->subch.prev.byTrackNum + 1, __LINE__);
						bFix = TRUE;
					}
				}
				if (!bFix) {
					if (pDiscPerSector->subch.prev.byAdr == ADR_ENCODES_CURRENT_POSITION) {
						if (pDiscPerSector->subch.prev.byIndex != 0 && pDiscPerSector->subch.current.byIndex == 1 && pDiscPerSector->subch.current.nRelativeTime == 0) {
							// Bikkuriman Daijikai (Japan)
							// LBA[106402, 0x19FA2], Audio, 2ch, Copy NG, Pre-emphasis No, Track[70], Idx[01], RMSF[00:16:39], AMSF[23:40:52], RtoW[0, 0, 0, 0]
							// LBA[106403, 0x19FA3], Audio, 2ch, Copy NG, Pre-emphasis No, Track[79], Idx[01], RMSF[00:00:00], AMSF[21:40:53], RtoW[0, 0, 0, 0]
							// LBA[106404, 0x19FA4], Audio, 2ch, Copy NG, Pre-emphasis No, Track[71], Idx[01], RMSF[00:00:01], AMSF[23:40:54], RtoW[0, 0, 0, 0]
							pDiscPerSector->subch.current.byTrackNum = (BYTE)(pDiscPerSector->subch.prev.byTrackNum + 1);
							OutputSubErrorLog("[%02d], [L:%d]\n", pDiscPerSector->subch.prev.byTrackNum + 1, __LINE__);
						}
						else {
							if (pDiscPerSector->subch.prev.byAdr == ADR_ENCODES_CURRENT_POSITION) {
								pDiscPerSector->subch.current.byTrackNum = pDiscPerSector->subch.prev.byTrackNum;
								OutputSubErrorLog("[%02u], [L:%d]\n", pDiscPerSector->subch.prev.byTrackNum, __LINE__);
							}
						}
					}
					else if (pDiscPerSector->subch.prevPrev.byAdr == ADR_ENCODES_CURRENT_POSITION) {
						pDiscPerSector->subch.current.byTrackNum = pDiscPerSector->subch.prevPrev.byTrackNum;
						OutputSubErrorLog("[%02u], [L:%d]\n", pDiscPerSector->subch.prevPrev.byTrackNum, __LINE__);
					}
				}

				if (pDiscPerSector->subch.current.byTrackNum == 110) {
					pDiscPerSector->subcode.current[13] = 0xaa;
				}
				else {
					pDiscPerSector->subcode.current[13] = DecToBcd(pDiscPerSector->subch.current.byTrackNum);
				}
				RecalcSubQCrc(pDisc, pDiscPerSector);
				if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
					return;
				}
			}
		}
		else if (!bPrevTrackNum) {
			if (pDiscPerSector->subch.prev.byIndex < MAXIMUM_NUMBER_INDEXES &&
				0 < pDiscPerSector->subch.prev.byTrackNum && pDiscPerSector->subch.prev.byTrackNum <= pDisc->SCSI.toc.LastTrack) {
				OutputSubErrorWithLBALog("Q[13]:PrevTrackNum[%02u] -> [%02u]\n", nLBA, pDiscPerSector->byTrackNum
					, pDiscPerSector->subch.prev.byTrackNum, pDiscPerSector->subch.prevPrev.byTrackNum);
				pDisc->SUB.lp1stLBAListOnSub[pDiscPerSector->subch.prev.byTrackNum - 1][pDiscPerSector->subch.prev.byIndex] = -1;
				pDisc->SUB.lp1stLBAListOnSubSync[pDiscPerSector->subch.prev.byTrackNum - 1][pDiscPerSector->subch.prev.byIndex] = -1;
			}
			pDiscPerSector->subch.prev.byTrackNum = pDiscPerSector->subch.prevPrev.byTrackNum;
			pDiscPerSector->subch.prev.byIndex = pDiscPerSector->subch.prevPrev.byIndex;
			pDiscPerSector->subch.prev.nRelativeTime = pDiscPerSector->subch.prevPrev.nRelativeTime + 1;
		}

		BOOL bPrevIndex = TRUE;
		BOOL bPrevPrevIndex = TRUE;
		if (!IsValidSubQIdx(pDisc, pDiscPerSector, nLBA, &bPrevIndex, &bPrevPrevIndex)) {
			BYTE tmpIdx = pDiscPerSector->subch.prev.byIndex;
			if (pDiscPerSector->subch.prev.byAdr != ADR_ENCODES_CURRENT_POSITION) {
				tmpIdx = pDiscPerSector->subch.prevPrev.byIndex;
			}

			if (pDiscPerSector->byTrackNum > 0 && nLBA == pDisc->SCSI.lp1stLBAListOnToc[pDiscPerSector->byTrackNum - 1]) {
				tmpIdx = 1;
			}
//			else if (IsValidPregapSector(pDisc, &pDiscPerSector->subch, nLBA)) {
				if (pDiscPerSector->subch.next.nRelativeTime + 1 == pDiscPerSector->subch.current.nRelativeTime) {
					tmpIdx = 0;
				}
//			}
			OutputSubErrorWithLBALog("Q[14]:Idx[%02u] -> [%02u], [L:%ld]\n"
				, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.current.byIndex, tmpIdx, s_lineNum);
			pDiscPerSector->subch.current.byIndex = tmpIdx;
			pDiscPerSector->subcode.current[14] = DecToBcd(pDiscPerSector->subch.current.byIndex);

			RecalcSubQCrc(pDisc, pDiscPerSector);
			if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
				return;
			}
		}
		else if (!bPrevIndex) {
			if (pDiscPerSector->subch.prev.byIndex < MAXIMUM_NUMBER_INDEXES &&
				0 < pDiscPerSector->subch.prev.byTrackNum && pDiscPerSector->subch.prev.byTrackNum <= pDisc->SCSI.toc.LastTrack) {
				OutputSubErrorWithLBALog("Q[14]:PrevIdx[%02u] -> [%02u]\n"
					, nLBA - 1, pDiscPerSector->byTrackNum, pDiscPerSector->subch.prev.byIndex, pDiscPerSector->subch.prevPrev.byIndex);
				pDisc->SUB.lp1stLBAListOnSub[pDiscPerSector->subch.prev.byTrackNum - 1][pDiscPerSector->subch.prev.byIndex] = -1;
				pDisc->SUB.lp1stLBAListOnSubSync[pDiscPerSector->subch.prev.byTrackNum - 1][pDiscPerSector->subch.prev.byIndex] = -1;
			}
			pDiscPerSector->subch.prev.byTrackNum = pDiscPerSector->subch.prevPrev.byTrackNum;
			pDiscPerSector->subch.prev.byIndex = pDiscPerSector->subch.prevPrev.byIndex;
		}
		else if (!bPrevPrevIndex) {
			if (pDiscPerSector->subch.prevPrev.byIndex < MAXIMUM_NUMBER_INDEXES &&
				0 < pDiscPerSector->subch.prev.byTrackNum && pDiscPerSector->subch.prev.byTrackNum <= pDisc->SCSI.toc.LastTrack) {
				OutputSubErrorWithLBALog("Q[14]:PrevPrevIdx[%02u] -> [%02u]\n"
					, nLBA - 1, pDiscPerSector->byTrackNum, pDiscPerSector->subch.prevPrev.byIndex, pDiscPerSector->subch.prev.byIndex);
				pDisc->SUB.lp1stLBAListOnSub[pDiscPerSector->subch.prevPrev.byTrackNum - 1][pDiscPerSector->subch.prevPrev.byIndex] = -1;
				pDisc->SUB.lp1stLBAListOnSubSync[pDiscPerSector->subch.prevPrev.byTrackNum - 1][pDiscPerSector->subch.prevPrev.byIndex] = -1;
			}
			pDiscPerSector->subch.prevPrev.byTrackNum = pDiscPerSector->subch.prev.byTrackNum;
			pDiscPerSector->subch.prevPrev.byIndex = pDiscPerSector->subch.prev.byIndex;
		}

		if (!IsValidSubQRMSF(pExecType, pDiscPerSector, nLBA)) {
			BYTE byFrame = 0;
			BYTE bySecond = 0;
			BYTE byMinute = 0;
			BYTE byPrevFrame = 0;
			BYTE byPrevSecond = 0;
			BYTE byPrevMinute = 0;
			INT tmpRel = 0;
			if (pDiscPerSector->subch.prev.byAdr == ADR_ENCODES_CURRENT_POSITION) {
				if ((!(pDiscPerSector->subch.prev.byIndex == 0 && pDiscPerSector->subch.current.byIndex == 1) &&
					!(pDiscPerSector->subch.prev.byIndex >= 1 && pDiscPerSector->subch.current.byIndex == 0)) ||
					(nLBA == 0 && (pDisc->PROTECT.byExist == securomV3_1 || pDisc->PROTECT.byExist == securomV3_2 || pDisc->PROTECT.byExist == securomV3_3))) {
					if (pDiscPerSector->subch.current.byIndex > 0) {
						if (pDisc->SCSI.lp1stLBAListOnToc[pDiscPerSector->byTrackNum] != nLBA) {
							tmpRel = pDiscPerSector->subch.prev.nRelativeTime + 1;
						}
					}
					else if (pDiscPerSector->subch.current.byIndex == 0) {
						tmpRel = pDiscPerSector->subch.prev.nRelativeTime - 1;
					}
					// Colin McRae Rally 2.0 (Europe) (En,Fr,De,Es,It) etc
					if (pDiscPerSector->bSecuRom && pDisc->PROTECT.byExist == securomV3_1 && nLBA == 7) {
						tmpRel -= 1;
					}
					LBAtoMSF(tmpRel, &byMinute, &bySecond, &byFrame);
					LBAtoMSF(pDiscPerSector->subch.prev.nRelativeTime, &byPrevMinute, &byPrevSecond, &byPrevFrame);
					OutputSubErrorWithLBALog(
						"Q[15-17]:PrevRel[%d, %02u:%02u:%02u], Rel[%d, %02u:%02u:%02u] -> [%d, %02u:%02u:%02u], [L:%ld]"
						, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.prev.nRelativeTime, byPrevMinute, byPrevSecond, byPrevFrame
						, pDiscPerSector->subch.current.nRelativeTime, BcdToDec(pDiscPerSector->subcode.current[15])
						, BcdToDec(pDiscPerSector->subcode.current[16]), BcdToDec(pDiscPerSector->subcode.current[17])
						, tmpRel, byMinute, bySecond, byFrame, s_lineNum);
					if (pDiscPerSector->bLibCrypt || pDiscPerSector->bSecuRom) {
						OutputSubErrorLog(
							" But this sector may be the intentional error of RMSF. see _subinfo.txt");
						INT nMax = 8;
						if (nLBA < 8) {
							nMax = 7;
						}
						if (0 < pDisc->PROTECT.byRestoreCounter && pDisc->PROTECT.byRestoreCounter < nMax &&
							pDiscPerSector->subch.prev.nRelativeTime + 2 != pDiscPerSector->subch.current.nRelativeTime) {
							LBAtoMSF(pDiscPerSector->subch.prev.nRelativeTime + 2, &byPrevMinute, &byPrevSecond, &byPrevFrame);
							SubQcodeOrg[3] = DecToBcd(byPrevMinute);
							SubQcodeOrg[4] = DecToBcd(byPrevSecond);
							SubQcodeOrg[5] = DecToBcd(byPrevFrame);
							OutputSubErrorLog(" And this RMSF is a random error. Fixed [%d, %02u:%02u:%02u]\n"
								, pDiscPerSector->subch.prev.nRelativeTime + 2, byPrevMinute, byPrevSecond, byPrevFrame);
						}
						else {
							OutputSubErrorLog("\n");
						}
					}
					else {
						OutputSubErrorLog("\n");
					}
					pDiscPerSector->subch.current.nRelativeTime = tmpRel;
					pDiscPerSector->subcode.current[15] = DecToBcd(byMinute);
					pDiscPerSector->subcode.current[16] = DecToBcd(bySecond);
					pDiscPerSector->subcode.current[17] = DecToBcd(byFrame);

					RecalcSubQCrc(pDisc, pDiscPerSector);
					if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
						return;
					}
				}
			}
			else {
				if (!(pDiscPerSector->subch.prevPrev.byIndex == 0 && pDiscPerSector->subch.current.byIndex == 1) &&
					!(pDiscPerSector->subch.prevPrev.byIndex >= 1 && pDiscPerSector->subch.current.byIndex == 0)) {
					if (pDiscPerSector->subch.current.byIndex > 0) {
						if (pDisc->SCSI.lp1stLBAListOnToc[pDiscPerSector->byTrackNum] != nLBA) {
							tmpRel = pDiscPerSector->subch.prevPrev.nRelativeTime + 2;
						}
					}
					else if (pDiscPerSector->subch.current.byIndex == 0) {
						tmpRel = pDiscPerSector->subch.prevPrev.nRelativeTime - 2;
					}
					LBAtoMSF(tmpRel, &byMinute, &bySecond, &byFrame);
					LBAtoMSF(pDiscPerSector->subch.prev.nRelativeTime, &byPrevMinute, &byPrevSecond, &byPrevFrame);
					OutputSubErrorWithLBALog(
						"Q[15-17]:PrevPrevRel[%d, %02u:%02u:%02u], Rel[%d, %02u:%02u:%02u] -> [%d, %02u:%02u:%02u], [L:%ld]\n"
						, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.prevPrev.nRelativeTime, byPrevMinute, byPrevSecond, byPrevFrame
						, pDiscPerSector->subch.current.nRelativeTime, BcdToDec(pDiscPerSector->subcode.current[15])
						, BcdToDec(pDiscPerSector->subcode.current[16]), BcdToDec(pDiscPerSector->subcode.current[17])
						, tmpRel, byMinute, bySecond, byFrame, s_lineNum);
					pDiscPerSector->subch.current.nRelativeTime = tmpRel;
					pDiscPerSector->subcode.current[15] = DecToBcd(byMinute);
					pDiscPerSector->subcode.current[16] = DecToBcd(bySecond);
					pDiscPerSector->subcode.current[17] = DecToBcd(byFrame);

					RecalcSubQCrc(pDisc, pDiscPerSector);
					if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
						return;
					}
				}
			}
		}

		if (pDiscPerSector->subcode.current[18] != 0) {
			OutputSubErrorWithLBALog("Q[18]:[%#04x] -> [0x00]\n"
				, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subcode.current[18]);
			pDiscPerSector->subcode.current[18] = 0;

			RecalcSubQCrc(pDisc, pDiscPerSector);
			if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
				return;
			}
		}

		if (!IsValidSubQAMSF(pExecType, pExtArg->byPre, pDiscPerSector, nLBA)) {
			BYTE byFrame = 0;
			BYTE bySecond = 0;
			BYTE byMinute = 0;
			INT tmpAbs = nLBA + 150;
			LBAtoMSF(tmpAbs, &byMinute, &bySecond, &byFrame);
			BYTE byPrevFrame = 0;
			BYTE byPrevSecond = 0;
			BYTE byPrevMinute = 0;
			LBAtoMSF(pDiscPerSector->subch.prev.nAbsoluteTime, &byPrevMinute, &byPrevSecond, &byPrevFrame);
			OutputSubErrorWithLBALog(
				"Q[19-21]:PrevAbs[%d, %02u:%02u:%02u], Abs[%d, %02u:%02u:%02u] -> [%d, %02u:%02u:%02u]"
				, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subch.prev.nAbsoluteTime
				, byPrevMinute, byPrevSecond, byPrevFrame
				, pDiscPerSector->subch.current.nAbsoluteTime, BcdToDec(pDiscPerSector->subcode.current[19])
				, BcdToDec(pDiscPerSector->subcode.current[20]), BcdToDec(pDiscPerSector->subcode.current[21])
				, tmpAbs, byMinute, bySecond, byFrame);

			if (pDiscPerSector->bLibCrypt || pDiscPerSector->bSecuRom) {
				OutputSubErrorLog(
					" But this sector may be the intentional error of AMSF. see _subinfo.txt");
				INT nMax = 8;
				if (nLBA < 8) {
					nMax = 7;
				}
				if (0 < pDisc->PROTECT.byRestoreCounter && pDisc->PROTECT.byRestoreCounter < nMax &&
					pDiscPerSector->subch.prev.nAbsoluteTime + 2 != pDiscPerSector->subch.current.nAbsoluteTime) {
					LBAtoMSF(pDiscPerSector->subch.prev.nAbsoluteTime + 2, &byPrevMinute, &byPrevSecond, &byPrevFrame);
					SubQcodeOrg[7] = DecToBcd(byPrevMinute);
					SubQcodeOrg[8] = DecToBcd(byPrevSecond);
					SubQcodeOrg[9] = DecToBcd(byPrevFrame);
					OutputSubErrorLog(" And this AMSF is a random error. Fixed [%d, %02u:%02u:%02u]\n"
						, pDiscPerSector->subch.prev.nAbsoluteTime + 2, byPrevMinute, byPrevSecond, byPrevFrame);
				}
				else {
					OutputSubErrorLog("\n");
				}
			}
			else {
				OutputSubErrorLog("\n");
			}
			pDiscPerSector->subch.current.nAbsoluteTime = MSFtoLBA(byMinute, bySecond, byFrame)/* + 150*/;
			pDiscPerSector->subcode.current[19] = DecToBcd(byMinute);
			pDiscPerSector->subcode.current[20] = DecToBcd(bySecond);
			pDiscPerSector->subcode.current[21] = DecToBcd(byFrame);

			RecalcSubQCrc(pDisc, pDiscPerSector);
			if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
				return;
			}
		}
	}
	else if (nLBA >= 0 &&
		(pDiscPerSector->subch.current.byAdr == ADR_ENCODES_MEDIA_CATALOG ||
		pDiscPerSector->subch.current.byAdr == ADR_ENCODES_ISRC ||
		pDiscPerSector->subch.current.byAdr == ADR_ENCODES_6)) {
		if (!IsValidSubQAFrame(pDiscPerSector->subcode.current, nLBA)) {
			BYTE byFrame = 0;
			BYTE bySecond = 0;
			BYTE byMinute = 0;
			LBAtoMSF(nLBA + 150, &byMinute, &bySecond, &byFrame);
			BYTE byPrevFrame = 0;
			BYTE byPrevSecond = 0;
			BYTE byPrevMinute = 0;
			LBAtoMSF(pDiscPerSector->subch.prev.nAbsoluteTime, &byPrevMinute, &byPrevSecond, &byPrevFrame);
			OutputSubErrorWithLBALog(
				"Q[21]:PrevAbsFrame[%02u], AbsFrame[%02u] -> [%02u]\n", nLBA, pDiscPerSector->byTrackNum
				, byPrevFrame, BcdToDec(pDiscPerSector->subcode.current[21]), byFrame);
			pDiscPerSector->subch.current.nAbsoluteTime = MSFtoLBA(byMinute, bySecond, byFrame)/* + 150*/;
			pDiscPerSector->subcode.current[21] = DecToBcd(byFrame);

			RecalcSubQCrc(pDisc, pDiscPerSector);
			if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
				return;
			}
		}
	}

	if (pDiscPerSector->byTrackNum > 0 && !IsValidSubQCtl(&pDiscPerSector->subch, pDisc->SUB.lpEndCtlList[pDiscPerSector->byTrackNum - 1])) {
		OutputSubErrorWithLBALog("Q[12]:Ctl[%hhu] -> [%hhu], [L:%ld]\n", nLBA, pDiscPerSector->byTrackNum
			, pDiscPerSector->subch.current.byCtl, pDiscPerSector->subch.prev.byCtl, s_lineNum);
		pDiscPerSector->subch.current.byCtl = pDiscPerSector->subch.prev.byCtl;
		pDiscPerSector->subcode.current[12] =
			(BYTE)(pDiscPerSector->subch.current.byCtl << 4 | pDiscPerSector->subch.current.byAdr);

		RecalcSubQCrc(pDisc, pDiscPerSector);
		if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
			return;
		}
	}

	// pDiscPerSector->subcode.current has already fixed random errors (= original crc)
	WORD crc16 = (WORD)GetCrc16CCITT(10, &pDiscPerSector->subcode.current[12]);
	if (pDiscPerSector->bLibCrypt || pDiscPerSector->bSecuRom) {
		BOOL bExist = FALSE;
		WORD xorCrc16 = (WORD)(crc16 ^ 0x8001);
		if (SubQcodeOrg[10] == HIBYTE(xorCrc16) && SubQcodeOrg[11] == LOBYTE(xorCrc16)) {
			OutputSubInfoWithLBALog(
				"Detected intentional error. CRC-16 is original:[%04x] and XORed with 0x8001:[%04x]"
				, nLBA, pDiscPerSector->byTrackNum, crc16, xorCrc16);
			bExist = TRUE;
		}
		else {
			// pDiscPerSector->subcode.current isn't fixed (= recalc crc)
			WORD reCalcCrc16 = (WORD)GetCrc16CCITT(10, &SubQcodeOrg[0]);
			WORD reCalcXorCrc16 = (WORD)(reCalcCrc16 ^ 0x0080);
			if (SubQcodeOrg[10] == HIBYTE(reCalcXorCrc16) &&
				SubQcodeOrg[11] == LOBYTE(reCalcXorCrc16) &&
				pDisc->PROTECT.byRestoreCounter == 8) {
				OutputSubInfoWithLBALog(
					"Detected intentional error. CRC-16 is recalculated:[%04x] and XORed with 0x0080:[%04x]"
					, nLBA, pDiscPerSector->byTrackNum, reCalcCrc16, reCalcXorCrc16);
				bExist = TRUE;
			}
		}
		if (bExist) {
			OutputSubInfoLog(
				" Restore RMSF[%02x:%02x:%02x to %02x:%02x:%02x] AMSF[%02x:%02x:%02x to %02x:%02x:%02x]\n"
				, pDiscPerSector->subcode.current[15], pDiscPerSector->subcode.current[16]
				, pDiscPerSector->subcode.current[17], SubQcodeOrg[3], SubQcodeOrg[4], SubQcodeOrg[5]
				, pDiscPerSector->subcode.current[19], pDiscPerSector->subcode.current[20]
				, pDiscPerSector->subcode.current[21], SubQcodeOrg[7], SubQcodeOrg[8], SubQcodeOrg[9]);
			// rmsf
			pDiscPerSector->subcode.current[15] = SubQcodeOrg[3];
			pDiscPerSector->subcode.current[16] = SubQcodeOrg[4];
			pDiscPerSector->subcode.current[17] = SubQcodeOrg[5];
			// amsf
			pDiscPerSector->subcode.current[19] = SubQcodeOrg[7];
			pDiscPerSector->subcode.current[20] = SubQcodeOrg[8];
			pDiscPerSector->subcode.current[21] = SubQcodeOrg[9];
			// crc
			pDiscPerSector->subcode.current[22] = SubQcodeOrg[10];
			pDiscPerSector->subcode.current[23] = SubQcodeOrg[11];
			OutputIntentionalSubchannel(nLBA, &pDiscPerSector->subcode.current[12]);
			pDisc->PROTECT.byRestoreCounter = 0;
			pDisc->SUB.nCorruptCrcH = 0;
			pDisc->SUB.nCorruptCrcL = 0;
		}
		else {
			if (pDisc->PROTECT.byExist == securomV1 || pDisc->PROTECT.byExist == securomV2 ||
				pDisc->PROTECT.byExist == securomV3_1 || pDisc->PROTECT.byExist == securomV3_2 || pDisc->PROTECT.byExist == securomV3_3) {
				INT nPrevRMSF = MSFtoLBA(BcdToDec(pDiscPerSector->subcode.current[15])
					, BcdToDec(pDiscPerSector->subcode.current[16]), BcdToDec(pDiscPerSector->subcode.current[17]));
				INT nRMSF = MSFtoLBA(BcdToDec(SubQcodeOrg[3]), BcdToDec(SubQcodeOrg[4]), BcdToDec(SubQcodeOrg[5]));
				INT nPrevAMSF = MSFtoLBA(BcdToDec(pDiscPerSector->subcode.current[19])
					, BcdToDec(pDiscPerSector->subcode.current[20]), BcdToDec(pDiscPerSector->subcode.current[21]));
				INT nAMSF = MSFtoLBA(BcdToDec(SubQcodeOrg[7]), BcdToDec(SubQcodeOrg[8]), BcdToDec(SubQcodeOrg[9]));

				if ((nPrevRMSF + 1 == nRMSF && nPrevAMSF + 1 == nAMSF) ||
					(nPrevRMSF == nRMSF && nPrevAMSF + 1 == nAMSF && 0 <= nLBA && nLBA < 9 &&
					(pDisc->PROTECT.byExist == securomV3_1 || pDisc->PROTECT.byExist == securomV3_2 || pDisc->PROTECT.byExist == securomV3_3))) {
					OutputSubInfoWithLBALog(
						"Detected shifted sub. Restore RMSF[%02x:%02x:%02x to %02x:%02x:%02x] AMSF[%02x:%02x:%02x to %02x:%02x:%02x]\n"
						, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subcode.current[15], pDiscPerSector->subcode.current[16]
						, pDiscPerSector->subcode.current[17], SubQcodeOrg[3], SubQcodeOrg[4], SubQcodeOrg[5]
						, pDiscPerSector->subcode.current[19], pDiscPerSector->subcode.current[20], pDiscPerSector->subcode.current[21]
						, SubQcodeOrg[7], SubQcodeOrg[8], SubQcodeOrg[9]);
					// rmsf
					pDiscPerSector->subcode.current[15] = SubQcodeOrg[3];
					pDiscPerSector->subcode.current[16] = SubQcodeOrg[4];
					pDiscPerSector->subcode.current[17] = SubQcodeOrg[5];
					// amsf
					pDiscPerSector->subcode.current[19] = SubQcodeOrg[7];
					pDiscPerSector->subcode.current[20] = SubQcodeOrg[8];
					pDiscPerSector->subcode.current[21] = SubQcodeOrg[9];
					OutputIntentionalSubchannel(nLBA, &pDiscPerSector->subcode.current[12]);
					pDisc->PROTECT.byRestoreCounter++;
					pDisc->SUB.nCorruptCrcH = 0;
					pDisc->SUB.nCorruptCrcL = 0;
				}
				else {
					OutputSubInfoWithLBALog("Intentional error doesn't exist.\n", nLBA, pDiscPerSector->byTrackNum);
				}
			}
			else {
				OutputSubInfoWithLBALog("Intentional error doesn't exist.\n", nLBA, pDiscPerSector->byTrackNum);
			}
		}
	}
	else {
		BYTE tmp1 = HIBYTE(crc16);
		BYTE tmp2 = LOBYTE(crc16);
		pDisc->SUB.nCorruptCrcH = pDiscPerSector->subcode.current[22] == HIBYTE(crc16) ? FALSE : TRUE;
		pDisc->SUB.nCorruptCrcL = pDiscPerSector->subcode.current[23] == LOBYTE(crc16) ? FALSE : TRUE;

		// re-check track num
		if (pDisc->SUB.nCorruptCrcH && pDisc->SUB.nCorruptCrcL) {
			if ((*pExecType == swap && pDiscPerSector->subch.prev.byTrackNum + 1 == pDiscPerSector->subch.current.byTrackNum) ||
				pDiscPerSector->subch.current.byTrackNum == 110) {
				BOOL bResetTrack = FALSE;
				if (pDiscPerSector->subch.current.byTrackNum != pDiscPerSector->subch.prev.byTrackNum) {
					OutputSubErrorWithLBALog("Q CurrentTrackNum[%d] -> [%d]\n"
						, nLBA, pDiscPerSector->byTrackNum
						, pDiscPerSector->subch.current.byTrackNum, pDiscPerSector->subch.prev.byTrackNum);
					pDiscPerSector->subch.current.byTrackNum = pDiscPerSector->subch.prev.byTrackNum;
					bResetTrack = TRUE;
				}
				BOOL bResetSubQTrack = FALSE;
				if (pDiscPerSector->subch.current.byTrackNum != 110) {
					OutputSubErrorWithLBALog("Q[13]:[%#04x] -> [%#04x]\n"
						, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subcode.current[13]
						, pDiscPerSector->subch.prev.byTrackNum);
					pDiscPerSector->subcode.current[13] = DecToBcd(pDiscPerSector->subch.prev.byTrackNum);
					bResetSubQTrack = TRUE;
				}
				else {
					if (pDiscPerSector->subcode.current[13] != 0xaa) {
						pDiscPerSector->subcode.current[13] = 0xaa;
						OutputSubErrorWithLBALog("Q[13]:[%#04x] -> [0xaa]\n"
							, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subcode.current[13]);
						bResetSubQTrack = TRUE;
					}
				}
				if (bResetSubQTrack) {
					crc16 = RecalcSubQCrc(pDisc, pDiscPerSector);
				}
				if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL && (bResetTrack || bResetSubQTrack)) {
//					OutputSubErrorLog("\n");
				}
				else {
					if (pDisc->SUB.nCorruptCrcH) {
						OutputSubErrorWithLBALog("Q[22]:CrcHigh[%#04x] -> [%#04x]\n"
							, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subcode.current[22], HIBYTE(crc16));
						pDiscPerSector->subcode.current[22] = HIBYTE(crc16);
					}
					if (pDisc->SUB.nCorruptCrcL) {
						OutputSubErrorWithLBALog("Q[23]:CrcLow[%#04x] -> [%#04x]\n"
							, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subcode.current[23], LOBYTE(crc16));
						pDiscPerSector->subcode.current[23] = LOBYTE(crc16);
					}
				}
			}
			else {
				OutputSubErrorWithLBALog("Q[22]:CrcHigh[%#04x] -> [%#04x]\n"
					, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subcode.current[22], tmp1);
				pDiscPerSector->subcode.current[22] = tmp1;
				OutputSubErrorWithLBALog("Q[23]:CrcLow[%#04x] -> [%#04x]\n"
					, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subcode.current[23], tmp2);
				pDiscPerSector->subcode.current[23] = tmp2;
			}
		}
		else {
			if (pDisc->SUB.nCorruptCrcH) {
				OutputSubErrorWithLBALog("Q[22]:CrcHigh[%#04x] -> [%#04x]\n"
					, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subcode.current[22], tmp1);
				pDiscPerSector->subcode.current[22] = tmp1;
			}
			if (pDisc->SUB.nCorruptCrcL) {
				OutputSubErrorWithLBALog("Q[23]:CrcLow[%#04x] -> [%#04x]\n"
					, nLBA, pDiscPerSector->byTrackNum, pDiscPerSector->subcode.current[23], tmp2);
				pDiscPerSector->subcode.current[23] = tmp2;
			}
		}
	}
	return;
}

VOID FixSubRtoW(
	PDEVICE pDevice,
	PDISC pDisc,
	PDISC_PER_SECTOR pDiscPerSector,
	INT nLBA
) {
#if 0
	BYTE pDiscPerSector->subcode.currentOrg[CD_RAW_READ_SUBCODE_SIZE] = {};
	memcpy(pDiscPerSector->subcode.currentOrg, pDiscPerSector->data.current + pDevice->TRANSFER.uiBufSubOffset, CD_RAW_READ_SUBCODE_SIZE);
	SUB_R_TO_W scRW[4] = {};
	BYTE tmpCode[24] = {};
	for (INT k = 0; k < 4; k++) {
		for (INT j = 0; j < 24; j++) {
			tmpCode[j] = (BYTE)(*(pDiscPerSector->subcode.currentOrg + (k * 24 + j)) & 0x3f);
		}
		memcpy(&scRW[k], tmpCode, sizeof(scRW[k]));
		if (scRW[k].parityQ[0] != 0 || scRW[k].parityQ[1] != 0) {
			for (INT m = 0; m < 24; m++) {
				BYTE crc6 = GetCrc6ITU(24, tmpCode, m);
				OutputSubInfoLog("%d=%02x. ", m, crc6);
			}
			OutputSubInfoLog("\n");
			OutputSubInfoWithLBALog(
				"Pack[%2d]: parityQ[0][%02x], parityQ[1][%02x]\n",
				nLBA, nLBA, pDiscPerSector->byTrackNum, k, scRW[k].parityQ[0], scRW[k].parityQ[1]);
		}
		if (scRW[k].parityP[0] != 0 || scRW[k].parityP[1] != 0 ||
			scRW[k].parityP[2] != 0 || scRW[k].parityP[3] != 0) {
			for (INT m = 0; m < 24; m++) {
				BYTE crc6 = GetCrc6ITU(24, tmpCode, m);
				OutputSubInfoLog("%d=%02x. ", m, crc6);
			}
			OutputSubInfoLog("\n");
			OutputSubInfoWithLBALog(
				"Pack[%2d]: parityP[0][%02x], parityP[1][%02x], parityP[2][%02x], parityP[3][%02x]\n",
				nLBA, nLBA, pDiscPerSector->byTrackNum, k, scRW[k].parityP[0], scRW[k].parityP[1], scRW[k].parityP[2], scRW[k].parityP[3]);
		}
	}
#else
	UNREFERENCED_PARAMETER(pDevice);
#endif
	INT idx = pDiscPerSector->byTrackNum - 1;
	if (pDisc->SUB.lpRtoWList[idx] == SUB_RTOW_TYPE::Zero ||
		pDisc->SUB.lpRtoWList[idx] == SUB_RTOW_TYPE::PSXSpecific) {
		for (INT j = 24; j < CD_RAW_READ_SUBCODE_SIZE; j++) {
			if (pDiscPerSector->subcode.current[j] != 0) {
				if ((24 <= j && j <= 34) ||
					(j == 35 && pDisc->SUB.lpRtoWList[idx] == SUB_RTOW_TYPE::Zero)) {
					OutputSubErrorWithLBALog("R[%02d]:[%#04x] -> [0x00]\n"
						, nLBA, pDiscPerSector->byTrackNum, j, pDiscPerSector->subcode.current[j]);
					pDiscPerSector->subcode.current[j] = 0;
				}
				else if ((36 <= j && j <= 46) ||
					(j == 47 && pDisc->SUB.lpRtoWList[idx] == SUB_RTOW_TYPE::Zero)) {
					OutputSubErrorWithLBALog("S[%02d]:[%#04x] -> [0x00]\n"
						, nLBA, pDiscPerSector->byTrackNum, j, pDiscPerSector->subcode.current[j]);
					pDiscPerSector->subcode.current[j] = 0;
				}
				else if ((48 <= j && j <= 58) ||
					(j == 59 && pDisc->SUB.lpRtoWList[idx] == SUB_RTOW_TYPE::Zero)) {
					OutputSubErrorWithLBALog("T[%02d]:[%#04x] -> [0x00]\n"
						, nLBA, pDiscPerSector->byTrackNum, j, pDiscPerSector->subcode.current[j]);
					pDiscPerSector->subcode.current[j] = 0;
				}
				else if ((60 <= j && j <= 70) ||
					(j == 71 && pDisc->SUB.lpRtoWList[idx] == SUB_RTOW_TYPE::Zero)) {
					OutputSubErrorWithLBALog("U[%02d]:[%#04x] -> [0x00]\n"
						, nLBA, pDiscPerSector->byTrackNum, j, pDiscPerSector->subcode.current[j]);
					pDiscPerSector->subcode.current[j] = 0;
				}
				else if ((72 <= j && j <= 82) ||
					(j == 83 && pDisc->SUB.lpRtoWList[idx] == SUB_RTOW_TYPE::Zero)) {
					OutputSubErrorWithLBALog("V[%02d]:[%#04x] -> [0x00]\n"
						, nLBA, pDiscPerSector->byTrackNum, j, pDiscPerSector->subcode.current[j]);
					pDiscPerSector->subcode.current[j] = 0;
				}
				else if ((84 <= j && j <= 94) ||
					(j == 95 && pDisc->SUB.lpRtoWList[idx] == SUB_RTOW_TYPE::Zero)) {
					OutputSubErrorWithLBALog("W[%02d]:[%#04x] -> [0x00]\n"
						, nLBA, pDiscPerSector->byTrackNum, j, pDiscPerSector->subcode.current[j]);
					pDiscPerSector->subcode.current[j] = 0;
				}
			}
		}
	}
	return;
}

BOOL FixSubQChannelUsingSubQ(
	PDISC_PER_SECTOR tmpSector,
	PDISC_PER_SECTOR pDiscPerSector,
	INT nLBA,
	LPCTSTR label
) {
	BOOL bSubOk = FALSE;
	memcpy(&pDiscPerSector->subcode.current[12], &tmpSector->subcode.current[12], 12);
	SetTmpSubchFromBuffer(&pDiscPerSector->subch.current, pDiscPerSector->subcode.current);
	OutputSubErrorWithLBALog("Q fixed using %s subQ [index %d]", nLBA, pDiscPerSector->byTrackNum, label, tmpSector->subch.current.byIndex);
	INT nTmpLBA = MSFtoLBA(BcdToDec(tmpSector->subcode.current[19]), BcdToDec(tmpSector->subcode.current[20]), BcdToDec(tmpSector->subcode.current[21])) - 150;
	if (nLBA != nTmpLBA) {
		OutputSubErrorLog(", But subQ is also bad\n");
	}
	else {
		OutputSubErrorLog("\n");
		bSubOk = TRUE;
	}
	return bSubOk;
}

BOOL FixSubChannel(
	PEXEC_TYPE pExecType,
	PEXT_ARG pExtArg,
	PDEVICE pDevice,
	PDISC pDisc,
	PDISC_PER_SECTOR pDiscPerSector,
	INT nLBA,
	LPBOOL bReread
) {
	if (pDisc->SUB.nSubChannelOffset) {
		SetTmpSubchFromBuffer(&pDiscPerSector->subch.next, pDiscPerSector->subcode.next);
		if (1 <= pExtArg->uiSubAddionalNum) {
			SetTmpSubchFromBuffer(&pDiscPerSector->subch.nextNext, pDiscPerSector->subcode.nextNext);
		}
	}
	else {
		if (1 <= pExtArg->uiSubAddionalNum) {
			SetTmpSubchFromBuffer(&pDiscPerSector->subch.next, pDiscPerSector->subcode.next);
			if (2 <= pExtArg->uiSubAddionalNum) {
				SetTmpSubchFromBuffer(&pDiscPerSector->subch.nextNext, pDiscPerSector->subcode.nextNext);
			}
		}
	}
	if (!pExtArg->bySkipSubQ) {
		RecalcSubQCrc(pDisc, pDiscPerSector);
		// Red Alert (Japan) -> 208050 - 208052 is same subQ, so crc16 doesn't check this
		// LBA[208049, 0x32cb1], Audio, 2ch, Copy NG, Pre-emphasis No, Track[56], Idx[01], RMSF[03:15:34], AMSF[46:15:74], RtoW[0, 0, 0, 0]
		// LBA[208050, 0x32cb2], Audio, 2ch, Copy NG, Pre-emphasis No, Track[56], Idx[01], RMSF[03:15:35], AMSF[46:16:00], RtoW[0, 0, 0, 0]
		// LBA[208051, 0x32cb3], Audio, 2ch, Copy NG, Pre-emphasis No, Track[56], Idx[01], RMSF[03:15:35], AMSF[46:16:00], RtoW[0, 0, 0, 0]
		// LBA[208052, 0x32cb4], Audio, 2ch, Copy NG, Pre-emphasis No, Track[56], Idx[01], RMSF[03:15:35], AMSF[46:16:00], RtoW[0, 0, 0, 0]
		// LBA[208053, 0x32cb5], Audio, 2ch, Copy NG, Pre-emphasis No, Track[56], Idx[01], RMSF[03:15:36], AMSF[46:16:03], RtoW[0, 0, 0, 0]
		// LBA[208054, 0x32cb6], Audio, 2ch, Copy NG, Pre-emphasis No, Track[56], Idx[01], RMSF[03:15:37], AMSF[46:16:04], RtoW[0, 0, 0, 0]
		BOOL bAMSF = IsValidSubQAMSF(pExecType, pExtArg->byPre, pDiscPerSector, nLBA);
		BOOL bAFrame = IsValidSubQAFrame(pDiscPerSector->subcode.current, nLBA);
#if 0
		if (-76 < nLBA) {
#endif
			BOOL bSubOk = FALSE;
			if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL && (bAMSF || bAFrame)) {
				if (*pExecType == swap) {
					if (pDiscPerSector->byTrackNum + 1 == pDiscPerSector->subch.current.byTrackNum) {
						pDiscPerSector->byTrackNum = pDiscPerSector->subch.current.byTrackNum;
					}
				}
				if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_MEDIA_CATALOG) {
					UpdateTmpSubchForMCN(pDisc, pDiscPerSector, nLBA);
					pDisc->SUB.nPrevMCNSector = nLBA;
					if (!pDisc->SUB.byCatalog) {
						CHAR szCatalog[META_CATALOG_SIZE] = {};
						SetMCNToString(pDisc, pDiscPerSector->subcode.current, szCatalog, TRUE);
						pDisc->SUB.byCatalog = TRUE;
					}
				}
				else if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_ISRC) {
					UpdateTmpSubchForISRC(&pDiscPerSector->subch);
					pDisc->SUB.nPrevISRCSector = nLBA;
					if (pDiscPerSector->byTrackNum > 0 && pDisc->SUB.lpISRCList[pDiscPerSector->byTrackNum - 1] == 0) {
						CHAR szISRC[META_ISRC_SIZE] = {};
						SetISRCToString(pDisc, pDiscPerSector, szISRC, TRUE);
					}
				}
				else if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_6) {
					UpdateTmpSubchForAdr6(pDisc, pDiscPerSector, nLBA);
				}

				if (*bReread) {
					OutputSubErrorLog("OK\n");
					*bReread = FALSE;
				}
				bSubOk = TRUE;
			}
			else if (!*bReread && !pDiscPerSector->bLibCrypt && !pDiscPerSector->bSecuRom) {
				DISC_PER_SECTOR tmpSector = {};
				memcpy(&tmpSector, pDiscPerSector, sizeof(DISC_PER_SECTOR));
				// fix for index 1 or higher
				tmpSector.subch.current = tmpSector.subch.next;
				tmpSector.subch.current.nRelativeTime -= 1;
				tmpSector.subch.current.nAbsoluteTime -= 1;
				SetBufferFromTmpSubch(tmpSector.subcode.current, tmpSector.subch.current, TRUE, FALSE);
				RecalcSubQCrc(pDisc, &tmpSector);

				if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL && tmpSector.subch.next.byAdr == ADR_ENCODES_CURRENT_POSITION) {
					bSubOk = FixSubQChannelUsingSubQ(&tmpSector, pDiscPerSector, nLBA, "next");
				}
				else {
					// fix for index 0
					tmpSector.subch.current = tmpSector.subch.next;
					tmpSector.subch.current.nRelativeTime += 1;
					tmpSector.subch.current.nAbsoluteTime -= 1;
					SetBufferFromTmpSubch(tmpSector.subcode.current, tmpSector.subch.current, TRUE, FALSE);
					RecalcSubQCrc(pDisc, &tmpSector);

					if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL && tmpSector.subch.next.byAdr == ADR_ENCODES_CURRENT_POSITION) {
						bSubOk = FixSubQChannelUsingSubQ(&tmpSector, pDiscPerSector, nLBA, "next");
					}
					else {
						tmpSector.subch.current = tmpSector.subch.prev;
						tmpSector.subch.current.nRelativeTime += 1;
						tmpSector.subch.current.nAbsoluteTime += 1;
						SetBufferFromTmpSubch(tmpSector.subcode.current, tmpSector.subch.current, TRUE, FALSE);
						RecalcSubQCrc(pDisc, &tmpSector);

						if (!pDisc->SUB.nCorruptCrcH && !pDisc->SUB.nCorruptCrcL) {
							bSubOk = FixSubQChannelUsingSubQ(&tmpSector, pDiscPerSector, nLBA, "prev");
						}
					}
				}
			}
			if (!bSubOk) {
				if (!*bReread && pDiscPerSector->bReturnCode != RETURNED_EXIST_C2_ERROR) {
					if (nLBA < MAX_LBA_OF_CD) {
						OutputSubErrorWithLBALog("Q Reread [crc16 unmatch] -> ", nLBA, pDiscPerSector->byTrackNum);
						*bReread = TRUE;
						return TRUE;
					}
					else {
						OutputSubErrorWithLBALog("Q [crc16 unmatch] Fix manually\n", nLBA, pDiscPerSector->byTrackNum);
					}
				}
				else {
					OutputSubErrorLog("NG. Fix manually\n");
//					OutputCDC2Error296(fileSubError, pDiscPerSector->data.current + 2352, nLBA);
//					OutputCDSub96Raw(fileSubError, pDiscPerSector->data.current + 2352 + 294, nLBA);
					OutputCDSub96Align(fileSubError, pDiscPerSector->subcode.current, nLBA);
					*bReread = FALSE;
				}

				if (pDiscPerSector->subcode.current[22] == 0 && pDiscPerSector->subcode.current[23] == 0) {
					OutputSubErrorWithLBALog("Q crc16 is 0. Main-channel may be corrupt\n", nLBA, pDiscPerSector->byTrackNum);
					*bReread = FALSE;
//					return FALSE;
				}
//				else {
					FixSubQ(pExecType, pExtArg, pDisc, pDiscPerSector, nLBA);
					if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_MEDIA_CATALOG) {
						UpdateTmpSubchForMCN(pDisc, pDiscPerSector, nLBA);
					}
					else if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_ISRC) {
						UpdateTmpSubchForISRC(&pDiscPerSector->subch);
					}
					else if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_6) {
						UpdateTmpSubchForAdr6(pDisc, pDiscPerSector, nLBA);
					}
//				}
			}
#if 0
		}
		else {
			if (pDisc->SUB.nCorruptCrcH || pDisc->SUB.nCorruptCrcL) {
				// TODO
				OutputSubErrorWithLBALog("Q <TODO>\n", nLBA, pDiscPerSector->byTrackNum);
				*bReread = FALSE;
			}
		}
#endif
	}
	else {
		// manually fix
#if 0
		if (pDiscPerSector->subch.current.byTrackNum > pDisc->SCSI.toc.LastTrack && pDiscPerSector->subch.current.byTrackNum != 110) {
			OutputErrorString("pDiscPerSector->subch.current.byTrackNum:%d, pDiscPerSector->subch.prev.byTrackNum:%d\n"
				, pDiscPerSector->subch.current.byTrackNum, pDiscPerSector->subch.prev.byTrackNum);
			pDiscPerSector->subch.current.byTrackNum = pDiscPerSector->subch.prev.byTrackNum;
		}
		RecalcSubQCrc(pDisc, pDiscPerSector);
		if (pDisc->SUB.nCorruptCrcH || pDisc->SUB.nCorruptCrcL) {
			FixSubQ(pExecType, pExtArg, pDisc, pDiscPerSector, nLBA);
		}
		if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_MEDIA_CATALOG) {
			UpdateTmpSubchForMCN(pExtArg, pDisc, pDiscPerSector, nLBA);
		}
		else if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_ISRC) {
			UpdateTmpSubchForISRC(&pDiscPerSector->subch);
		}
		else if (pDiscPerSector->subch.current.byAdr == ADR_ENCODES_6) {
			UpdateTmpSubchForAdr6(pDisc, pDiscPerSector, nLBA);
		}
#endif
	}
	if (!*bReread) {
		if (!pExtArg->bySkipSubP) {
			FixSubP(pDiscPerSector, nLBA);
		}
		if (!pExtArg->bySkipSubRtoW) {
			FixSubRtoW(pDevice, pDisc, pDiscPerSector, nLBA);
		}
	}
	return TRUE;
}
