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
#pragma once

BOOL AlignRowSubcode(
	LPBYTE lpRowSubcode,
	LPBYTE lpColumnSubcode
);

BOOL AlignColumnSubcode(
	LPBYTE lpColumnSubcode,
	LPBYTE lpRowSubcode
);

BYTE BcdToDec(
	BYTE bySrc
);

BYTE DecToBcd(
	BYTE bySrc
);

INT MSFtoLBA(
	BYTE byMinute,
	BYTE bySecond,
	BYTE byFrame
);

VOID LBAtoMSF(
	INT nLBA,
	LPBYTE byMinute,
	LPBYTE bySecond,
	LPBYTE byFrame
);

VOID LittleToBig(
	LPWCH pOut,
	LPWCH pIn,
	INT nCnt
);

LPVOID ConvParagraphBoundary(
	PDEVICE pDevice,
	LPBYTE pv
);

UINT PadSizeForVolDesc(
	UINT uiSize
);

INT ConvertUnicodeToSjis(
	LPCH pTmpText,
	LPCH* bufShiftJis,
	size_t stTmpTextLen,
	size_t stTxtIdx
);
