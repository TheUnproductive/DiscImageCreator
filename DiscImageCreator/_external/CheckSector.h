/*
Copyright(C) 2003, Y.Kanechika. All rights reserved.
Copyright(C) 2006, C-yan. All rights reserved.
�\�[�X�R�[�h�y�уo�C�i���`���̗��p�y�эĔz�z�́A�����̉��ςɊւ�炸�A�ȉ��̏����𖞂�������A������B

1.�\�[�X�R�[�h�̍Ĕz�z�́A��L�̒��쌠�\���A���̏�񃊃X�g�A����шȉ��̖Ɛӎ����𖾋L�������͂𔺂��āA�s��Ȃ���΂Ȃ�Ȃ��B
2.�o�C�i���`���̍Ĕz�z�́A��L�̒��쌠�\���A���̏�񃊃X�g�A�y�шȉ��̖Ɛӎ������A�z�z���ɕt������������A���͏��ނ֖��L���Ȃ���΂Ȃ�Ȃ��B
3.�\�[�X�R�[�h�y�уo�C�i���`���̍Ĕz�z�ɂ����āA�z�z�ɕK�v�ȕ����I�R�X�g������㉿�̗v���A���͂���ɗނ��郉�C�Z���X�s�ׂ��s���Ă͂Ȃ�Ȃ��B�i���Ɨ��p�͋֎~�j

���̃\�t�g�E�F�A�̗��p���甭�������A�����Ȃ���ɂ����Ă��A���̒��쌠�҂́A���Q�ɑ΂��ӔC�͂Ȃ��B
*/
#pragma once

BYTE* GetEccP();
BYTE* GetEccQ();
DWORD CalcEDC(BYTE* Buffer, DWORD size);
void CalcEccP(BYTE *buffer);
void CalcEccQ(BYTE *buffer);
int MemCmp(BYTE* Mem1, BYTE* Mem2, DWORD Count);
