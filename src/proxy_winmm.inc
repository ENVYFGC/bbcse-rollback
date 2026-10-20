constexpr int kWinmmExports = 193;
extern "C" FARPROC g_winmm[kWinmmExports] = {};
const char* const g_winmmName[kWinmmExports] = {
	nullptr,
	"mciExecute",
	"CloseDriver",
	"DefDriverProc",
	"DriverCallback",
	"DrvGetModuleHandle",
	"GetDriverModuleHandle",
	"NotifyCallbackData",
	"OpenDriver",
	"PlaySound",
	"PlaySoundA",
	"PlaySoundW",
	"SendDriverMessage",
	"WOW32DriverCallback",
	"WOW32ResolveMultiMediaHandle",
	"WOWAppExit",
	"aux32Message",
	"auxGetDevCapsA",
	"auxGetDevCapsW",
	"auxGetNumDevs",
	"auxGetVolume",
	"auxOutMessage",
	"auxSetVolume",
	"joy32Message",
	"joyConfigChanged",
	"joyGetDevCapsA",
	"joyGetDevCapsW",
	"joyGetNumDevs",
	"joyGetPos",
	"joyGetPosEx",
	"joyGetThreshold",
	"joyReleaseCapture",
	"joySetCapture",
	"joySetThreshold",
	"mci32Message",
	"mciDriverNotify",
	"mciDriverYield",
	"mciFreeCommandResource",
	"mciGetCreatorTask",
	"mciGetDeviceIDA",
	"mciGetDeviceIDFromElementIDA",
	"mciGetDeviceIDFromElementIDW",
	"mciGetDeviceIDW",
	"mciGetDriverData",
	"mciGetErrorStringA",
	"mciGetErrorStringW",
	"mciGetYieldProc",
	"mciLoadCommandResource",
	"mciSendCommandA",
	"mciSendCommandW",
	"mciSendStringA",
	"mciSendStringW",
	"mciSetDriverData",
	"mciSetYieldProc",
	"mid32Message",
	"midiConnect",
	"midiDisconnect",
	"midiInAddBuffer",
	"midiInClose",
	"midiInGetDevCapsA",
	"midiInGetDevCapsW",
	"midiInGetErrorTextA",
	"midiInGetErrorTextW",
	"midiInGetID",
	"midiInGetNumDevs",
	"midiInMessage",
	"midiInOpen",
	"midiInPrepareHeader",
	"midiInReset",
	"midiInStart",
	"midiInStop",
	"midiInUnprepareHeader",
	"midiOutCacheDrumPatches",
	"midiOutCachePatches",
	"midiOutClose",
	"midiOutGetDevCapsA",
	"midiOutGetDevCapsW",
	"midiOutGetErrorTextA",
	"midiOutGetErrorTextW",
	"midiOutGetID",
	"midiOutGetNumDevs",
	"midiOutGetVolume",
	"midiOutLongMsg",
	"midiOutMessage",
	"midiOutOpen",
	"midiOutPrepareHeader",
	"midiOutReset",
	"midiOutSetVolume",
	"midiOutShortMsg",
	"midiOutUnprepareHeader",
	"midiStreamClose",
	"midiStreamOpen",
	"midiStreamOut",
	"midiStreamPause",
	"midiStreamPosition",
	"midiStreamProperty",
	"midiStreamRestart",
	"midiStreamStop",
	"mixerClose",
	"mixerGetControlDetailsA",
	"mixerGetControlDetailsW",
	"mixerGetDevCapsA",
	"mixerGetDevCapsW",
	"mixerGetID",
	"mixerGetLineControlsA",
	"mixerGetLineControlsW",
	"mixerGetLineInfoA",
	"mixerGetLineInfoW",
	"mixerGetNumDevs",
	"mixerMessage",
	"mixerOpen",
	"mixerSetControlDetails",
	"mmDrvInstall",
	"mmGetCurrentTask",
	"mmTaskBlock",
	"mmTaskCreate",
	"mmTaskSignal",
	"mmTaskYield",
	"mmioAdvance",
	"mmioAscend",
	"mmioClose",
	"mmioCreateChunk",
	"mmioDescend",
	"mmioFlush",
	"mmioGetInfo",
	"mmioInstallIOProcA",
	"mmioInstallIOProcW",
	"mmioOpenA",
	"mmioOpenW",
	"mmioRead",
	"mmioRenameA",
	"mmioRenameW",
	"mmioSeek",
	"mmioSendMessage",
	"mmioSetBuffer",
	"mmioSetInfo",
	"mmioStringToFOURCCA",
	"mmioStringToFOURCCW",
	"mmioWrite",
	"mmsystemGetVersion",
	"mod32Message",
	"mxd32Message",
	"sndPlaySoundA",
	"sndPlaySoundW",
	"tid32Message",
	"timeBeginPeriod",
	"timeEndPeriod",
	"timeGetDevCaps",
	"timeGetSystemTime",
	"timeGetTime",
	"timeKillEvent",
	"timeSetEvent",
	"waveInAddBuffer",
	"waveInClose",
	"waveInGetDevCapsA",
	"waveInGetDevCapsW",
	"waveInGetErrorTextA",
	"waveInGetErrorTextW",
	"waveInGetID",
	"waveInGetNumDevs",
	"waveInGetPosition",
	"waveInMessage",
	"waveInOpen",
	"waveInPrepareHeader",
	"waveInReset",
	"waveInStart",
	"waveInStop",
	"waveInUnprepareHeader",
	"waveOutBreakLoop",
	"waveOutClose",
	"waveOutGetDevCapsA",
	"waveOutGetDevCapsW",
	"waveOutGetErrorTextA",
	"waveOutGetErrorTextW",
	"waveOutGetID",
	"waveOutGetNumDevs",
	"waveOutGetPitch",
	"waveOutGetPlaybackRate",
	"waveOutGetPosition",
	"waveOutGetVolume",
	"waveOutMessage",
	"waveOutOpen",
	"waveOutPause",
	"waveOutPrepareHeader",
	"waveOutReset",
	"waveOutRestart",
	"waveOutSetPitch",
	"waveOutSetPlaybackRate",
	"waveOutSetVolume",
	"waveOutUnprepareHeader",
	"waveOutWrite",
	"wid32Message",
	"wod32Message",
};
const WORD g_winmmOrd[kWinmmExports] = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194};
extern "C" __declspec(naked) void WinmmFwd0() { __asm jmp dword ptr [g_winmm + 0] }
extern "C" __declspec(naked) void WinmmFwd1() { __asm jmp dword ptr [g_winmm + 4] }
extern "C" __declspec(naked) void WinmmFwd2() { __asm jmp dword ptr [g_winmm + 8] }
extern "C" __declspec(naked) void WinmmFwd3() { __asm jmp dword ptr [g_winmm + 12] }
extern "C" __declspec(naked) void WinmmFwd4() { __asm jmp dword ptr [g_winmm + 16] }
extern "C" __declspec(naked) void WinmmFwd5() { __asm jmp dword ptr [g_winmm + 20] }
extern "C" __declspec(naked) void WinmmFwd6() { __asm jmp dword ptr [g_winmm + 24] }
extern "C" __declspec(naked) void WinmmFwd7() { __asm jmp dword ptr [g_winmm + 28] }
extern "C" __declspec(naked) void WinmmFwd8() { __asm jmp dword ptr [g_winmm + 32] }
extern "C" __declspec(naked) void WinmmFwd9() { __asm jmp dword ptr [g_winmm + 36] }
extern "C" __declspec(naked) void WinmmFwd10() { __asm jmp dword ptr [g_winmm + 40] }
extern "C" __declspec(naked) void WinmmFwd11() { __asm jmp dword ptr [g_winmm + 44] }
extern "C" __declspec(naked) void WinmmFwd12() { __asm jmp dword ptr [g_winmm + 48] }
extern "C" __declspec(naked) void WinmmFwd13() { __asm jmp dword ptr [g_winmm + 52] }
extern "C" __declspec(naked) void WinmmFwd14() { __asm jmp dword ptr [g_winmm + 56] }
extern "C" __declspec(naked) void WinmmFwd15() { __asm jmp dword ptr [g_winmm + 60] }
extern "C" __declspec(naked) void WinmmFwd16() { __asm jmp dword ptr [g_winmm + 64] }
extern "C" __declspec(naked) void WinmmFwd17() { __asm jmp dword ptr [g_winmm + 68] }
extern "C" __declspec(naked) void WinmmFwd18() { __asm jmp dword ptr [g_winmm + 72] }
extern "C" __declspec(naked) void WinmmFwd19() { __asm jmp dword ptr [g_winmm + 76] }
extern "C" __declspec(naked) void WinmmFwd20() { __asm jmp dword ptr [g_winmm + 80] }
extern "C" __declspec(naked) void WinmmFwd21() { __asm jmp dword ptr [g_winmm + 84] }
extern "C" __declspec(naked) void WinmmFwd22() { __asm jmp dword ptr [g_winmm + 88] }
extern "C" __declspec(naked) void WinmmFwd23() { __asm jmp dword ptr [g_winmm + 92] }
extern "C" __declspec(naked) void WinmmFwd24() { __asm jmp dword ptr [g_winmm + 96] }
extern "C" __declspec(naked) void WinmmFwd25() { __asm jmp dword ptr [g_winmm + 100] }
extern "C" __declspec(naked) void WinmmFwd26() { __asm jmp dword ptr [g_winmm + 104] }
extern "C" __declspec(naked) void WinmmFwd27() { __asm jmp dword ptr [g_winmm + 108] }
extern "C" __declspec(naked) void WinmmFwd28() { __asm jmp dword ptr [g_winmm + 112] }
extern "C" __declspec(naked) void WinmmFwd29() { __asm jmp dword ptr [g_winmm + 116] }
extern "C" __declspec(naked) void WinmmFwd30() { __asm jmp dword ptr [g_winmm + 120] }
extern "C" __declspec(naked) void WinmmFwd31() { __asm jmp dword ptr [g_winmm + 124] }
extern "C" __declspec(naked) void WinmmFwd32() { __asm jmp dword ptr [g_winmm + 128] }
extern "C" __declspec(naked) void WinmmFwd33() { __asm jmp dword ptr [g_winmm + 132] }
extern "C" __declspec(naked) void WinmmFwd34() { __asm jmp dword ptr [g_winmm + 136] }
extern "C" __declspec(naked) void WinmmFwd35() { __asm jmp dword ptr [g_winmm + 140] }
extern "C" __declspec(naked) void WinmmFwd36() { __asm jmp dword ptr [g_winmm + 144] }
extern "C" __declspec(naked) void WinmmFwd37() { __asm jmp dword ptr [g_winmm + 148] }
extern "C" __declspec(naked) void WinmmFwd38() { __asm jmp dword ptr [g_winmm + 152] }
extern "C" __declspec(naked) void WinmmFwd39() { __asm jmp dword ptr [g_winmm + 156] }
extern "C" __declspec(naked) void WinmmFwd40() { __asm jmp dword ptr [g_winmm + 160] }
extern "C" __declspec(naked) void WinmmFwd41() { __asm jmp dword ptr [g_winmm + 164] }
extern "C" __declspec(naked) void WinmmFwd42() { __asm jmp dword ptr [g_winmm + 168] }
extern "C" __declspec(naked) void WinmmFwd43() { __asm jmp dword ptr [g_winmm + 172] }
extern "C" __declspec(naked) void WinmmFwd44() { __asm jmp dword ptr [g_winmm + 176] }
extern "C" __declspec(naked) void WinmmFwd45() { __asm jmp dword ptr [g_winmm + 180] }
extern "C" __declspec(naked) void WinmmFwd46() { __asm jmp dword ptr [g_winmm + 184] }
extern "C" __declspec(naked) void WinmmFwd47() { __asm jmp dword ptr [g_winmm + 188] }
extern "C" __declspec(naked) void WinmmFwd48() { __asm jmp dword ptr [g_winmm + 192] }
extern "C" __declspec(naked) void WinmmFwd49() { __asm jmp dword ptr [g_winmm + 196] }
extern "C" __declspec(naked) void WinmmFwd50() { __asm jmp dword ptr [g_winmm + 200] }
extern "C" __declspec(naked) void WinmmFwd51() { __asm jmp dword ptr [g_winmm + 204] }
extern "C" __declspec(naked) void WinmmFwd52() { __asm jmp dword ptr [g_winmm + 208] }
extern "C" __declspec(naked) void WinmmFwd53() { __asm jmp dword ptr [g_winmm + 212] }
extern "C" __declspec(naked) void WinmmFwd54() { __asm jmp dword ptr [g_winmm + 216] }
extern "C" __declspec(naked) void WinmmFwd55() { __asm jmp dword ptr [g_winmm + 220] }
extern "C" __declspec(naked) void WinmmFwd56() { __asm jmp dword ptr [g_winmm + 224] }
extern "C" __declspec(naked) void WinmmFwd57() { __asm jmp dword ptr [g_winmm + 228] }
extern "C" __declspec(naked) void WinmmFwd58() { __asm jmp dword ptr [g_winmm + 232] }
extern "C" __declspec(naked) void WinmmFwd59() { __asm jmp dword ptr [g_winmm + 236] }
extern "C" __declspec(naked) void WinmmFwd60() { __asm jmp dword ptr [g_winmm + 240] }
extern "C" __declspec(naked) void WinmmFwd61() { __asm jmp dword ptr [g_winmm + 244] }
extern "C" __declspec(naked) void WinmmFwd62() { __asm jmp dword ptr [g_winmm + 248] }
extern "C" __declspec(naked) void WinmmFwd63() { __asm jmp dword ptr [g_winmm + 252] }
extern "C" __declspec(naked) void WinmmFwd64() { __asm jmp dword ptr [g_winmm + 256] }
extern "C" __declspec(naked) void WinmmFwd65() { __asm jmp dword ptr [g_winmm + 260] }
extern "C" __declspec(naked) void WinmmFwd66() { __asm jmp dword ptr [g_winmm + 264] }
extern "C" __declspec(naked) void WinmmFwd67() { __asm jmp dword ptr [g_winmm + 268] }
extern "C" __declspec(naked) void WinmmFwd68() { __asm jmp dword ptr [g_winmm + 272] }
extern "C" __declspec(naked) void WinmmFwd69() { __asm jmp dword ptr [g_winmm + 276] }
extern "C" __declspec(naked) void WinmmFwd70() { __asm jmp dword ptr [g_winmm + 280] }
extern "C" __declspec(naked) void WinmmFwd71() { __asm jmp dword ptr [g_winmm + 284] }
extern "C" __declspec(naked) void WinmmFwd72() { __asm jmp dword ptr [g_winmm + 288] }
extern "C" __declspec(naked) void WinmmFwd73() { __asm jmp dword ptr [g_winmm + 292] }
extern "C" __declspec(naked) void WinmmFwd74() { __asm jmp dword ptr [g_winmm + 296] }
extern "C" __declspec(naked) void WinmmFwd75() { __asm jmp dword ptr [g_winmm + 300] }
extern "C" __declspec(naked) void WinmmFwd76() { __asm jmp dword ptr [g_winmm + 304] }
extern "C" __declspec(naked) void WinmmFwd77() { __asm jmp dword ptr [g_winmm + 308] }
extern "C" __declspec(naked) void WinmmFwd78() { __asm jmp dword ptr [g_winmm + 312] }
extern "C" __declspec(naked) void WinmmFwd79() { __asm jmp dword ptr [g_winmm + 316] }
extern "C" __declspec(naked) void WinmmFwd80() { __asm jmp dword ptr [g_winmm + 320] }
extern "C" __declspec(naked) void WinmmFwd81() { __asm jmp dword ptr [g_winmm + 324] }
extern "C" __declspec(naked) void WinmmFwd82() { __asm jmp dword ptr [g_winmm + 328] }
extern "C" __declspec(naked) void WinmmFwd83() { __asm jmp dword ptr [g_winmm + 332] }
extern "C" __declspec(naked) void WinmmFwd84() { __asm jmp dword ptr [g_winmm + 336] }
extern "C" __declspec(naked) void WinmmFwd85() { __asm jmp dword ptr [g_winmm + 340] }
extern "C" __declspec(naked) void WinmmFwd86() { __asm jmp dword ptr [g_winmm + 344] }
extern "C" __declspec(naked) void WinmmFwd87() { __asm jmp dword ptr [g_winmm + 348] }
extern "C" __declspec(naked) void WinmmFwd88() { __asm jmp dword ptr [g_winmm + 352] }
extern "C" __declspec(naked) void WinmmFwd89() { __asm jmp dword ptr [g_winmm + 356] }
extern "C" __declspec(naked) void WinmmFwd90() { __asm jmp dword ptr [g_winmm + 360] }
extern "C" __declspec(naked) void WinmmFwd91() { __asm jmp dword ptr [g_winmm + 364] }
extern "C" __declspec(naked) void WinmmFwd92() { __asm jmp dword ptr [g_winmm + 368] }
extern "C" __declspec(naked) void WinmmFwd93() { __asm jmp dword ptr [g_winmm + 372] }
extern "C" __declspec(naked) void WinmmFwd94() { __asm jmp dword ptr [g_winmm + 376] }
extern "C" __declspec(naked) void WinmmFwd95() { __asm jmp dword ptr [g_winmm + 380] }
extern "C" __declspec(naked) void WinmmFwd96() { __asm jmp dword ptr [g_winmm + 384] }
extern "C" __declspec(naked) void WinmmFwd97() { __asm jmp dword ptr [g_winmm + 388] }
extern "C" __declspec(naked) void WinmmFwd98() { __asm jmp dword ptr [g_winmm + 392] }
extern "C" __declspec(naked) void WinmmFwd99() { __asm jmp dword ptr [g_winmm + 396] }
extern "C" __declspec(naked) void WinmmFwd100() { __asm jmp dword ptr [g_winmm + 400] }
extern "C" __declspec(naked) void WinmmFwd101() { __asm jmp dword ptr [g_winmm + 404] }
extern "C" __declspec(naked) void WinmmFwd102() { __asm jmp dword ptr [g_winmm + 408] }
extern "C" __declspec(naked) void WinmmFwd103() { __asm jmp dword ptr [g_winmm + 412] }
extern "C" __declspec(naked) void WinmmFwd104() { __asm jmp dword ptr [g_winmm + 416] }
extern "C" __declspec(naked) void WinmmFwd105() { __asm jmp dword ptr [g_winmm + 420] }
extern "C" __declspec(naked) void WinmmFwd106() { __asm jmp dword ptr [g_winmm + 424] }
extern "C" __declspec(naked) void WinmmFwd107() { __asm jmp dword ptr [g_winmm + 428] }
extern "C" __declspec(naked) void WinmmFwd108() { __asm jmp dword ptr [g_winmm + 432] }
extern "C" __declspec(naked) void WinmmFwd109() { __asm jmp dword ptr [g_winmm + 436] }
extern "C" __declspec(naked) void WinmmFwd110() { __asm jmp dword ptr [g_winmm + 440] }
extern "C" __declspec(naked) void WinmmFwd111() { __asm jmp dword ptr [g_winmm + 444] }
extern "C" __declspec(naked) void WinmmFwd112() { __asm jmp dword ptr [g_winmm + 448] }
extern "C" __declspec(naked) void WinmmFwd113() { __asm jmp dword ptr [g_winmm + 452] }
extern "C" __declspec(naked) void WinmmFwd114() { __asm jmp dword ptr [g_winmm + 456] }
extern "C" __declspec(naked) void WinmmFwd115() { __asm jmp dword ptr [g_winmm + 460] }
extern "C" __declspec(naked) void WinmmFwd116() { __asm jmp dword ptr [g_winmm + 464] }
extern "C" __declspec(naked) void WinmmFwd117() { __asm jmp dword ptr [g_winmm + 468] }
extern "C" __declspec(naked) void WinmmFwd118() { __asm jmp dword ptr [g_winmm + 472] }
extern "C" __declspec(naked) void WinmmFwd119() { __asm jmp dword ptr [g_winmm + 476] }
extern "C" __declspec(naked) void WinmmFwd120() { __asm jmp dword ptr [g_winmm + 480] }
extern "C" __declspec(naked) void WinmmFwd121() { __asm jmp dword ptr [g_winmm + 484] }
extern "C" __declspec(naked) void WinmmFwd122() { __asm jmp dword ptr [g_winmm + 488] }
extern "C" __declspec(naked) void WinmmFwd123() { __asm jmp dword ptr [g_winmm + 492] }
extern "C" __declspec(naked) void WinmmFwd124() { __asm jmp dword ptr [g_winmm + 496] }
extern "C" __declspec(naked) void WinmmFwd125() { __asm jmp dword ptr [g_winmm + 500] }
extern "C" __declspec(naked) void WinmmFwd126() { __asm jmp dword ptr [g_winmm + 504] }
extern "C" __declspec(naked) void WinmmFwd127() { __asm jmp dword ptr [g_winmm + 508] }
extern "C" __declspec(naked) void WinmmFwd128() { __asm jmp dword ptr [g_winmm + 512] }
extern "C" __declspec(naked) void WinmmFwd129() { __asm jmp dword ptr [g_winmm + 516] }
extern "C" __declspec(naked) void WinmmFwd130() { __asm jmp dword ptr [g_winmm + 520] }
extern "C" __declspec(naked) void WinmmFwd131() { __asm jmp dword ptr [g_winmm + 524] }
extern "C" __declspec(naked) void WinmmFwd132() { __asm jmp dword ptr [g_winmm + 528] }
extern "C" __declspec(naked) void WinmmFwd133() { __asm jmp dword ptr [g_winmm + 532] }
extern "C" __declspec(naked) void WinmmFwd134() { __asm jmp dword ptr [g_winmm + 536] }
extern "C" __declspec(naked) void WinmmFwd135() { __asm jmp dword ptr [g_winmm + 540] }
extern "C" __declspec(naked) void WinmmFwd136() { __asm jmp dword ptr [g_winmm + 544] }
extern "C" __declspec(naked) void WinmmFwd137() { __asm jmp dword ptr [g_winmm + 548] }
extern "C" __declspec(naked) void WinmmFwd138() { __asm jmp dword ptr [g_winmm + 552] }
extern "C" __declspec(naked) void WinmmFwd139() { __asm jmp dword ptr [g_winmm + 556] }
extern "C" __declspec(naked) void WinmmFwd140() { __asm jmp dword ptr [g_winmm + 560] }
extern "C" __declspec(naked) void WinmmFwd141() { __asm jmp dword ptr [g_winmm + 564] }
extern "C" __declspec(naked) void WinmmFwd142() { __asm jmp dword ptr [g_winmm + 568] }
extern "C" __declspec(naked) void WinmmFwd143() { __asm jmp dword ptr [g_winmm + 572] }
extern "C" __declspec(naked) void WinmmFwd144() { __asm jmp dword ptr [g_winmm + 576] }
extern "C" __declspec(naked) void WinmmFwd145() { __asm jmp dword ptr [g_winmm + 580] }
extern "C" __declspec(naked) void WinmmFwd146() { __asm jmp dword ptr [g_winmm + 584] }
extern "C" __declspec(naked) void WinmmFwd147() { __asm jmp dword ptr [g_winmm + 588] }
extern "C" __declspec(naked) void WinmmFwd148() { __asm jmp dword ptr [g_winmm + 592] }
extern "C" __declspec(naked) void WinmmFwd149() { __asm jmp dword ptr [g_winmm + 596] }
extern "C" __declspec(naked) void WinmmFwd150() { __asm jmp dword ptr [g_winmm + 600] }
extern "C" __declspec(naked) void WinmmFwd151() { __asm jmp dword ptr [g_winmm + 604] }
extern "C" __declspec(naked) void WinmmFwd152() { __asm jmp dword ptr [g_winmm + 608] }
extern "C" __declspec(naked) void WinmmFwd153() { __asm jmp dword ptr [g_winmm + 612] }
extern "C" __declspec(naked) void WinmmFwd154() { __asm jmp dword ptr [g_winmm + 616] }
extern "C" __declspec(naked) void WinmmFwd155() { __asm jmp dword ptr [g_winmm + 620] }
extern "C" __declspec(naked) void WinmmFwd156() { __asm jmp dword ptr [g_winmm + 624] }
extern "C" __declspec(naked) void WinmmFwd157() { __asm jmp dword ptr [g_winmm + 628] }
extern "C" __declspec(naked) void WinmmFwd158() { __asm jmp dword ptr [g_winmm + 632] }
extern "C" __declspec(naked) void WinmmFwd159() { __asm jmp dword ptr [g_winmm + 636] }
extern "C" __declspec(naked) void WinmmFwd160() { __asm jmp dword ptr [g_winmm + 640] }
extern "C" __declspec(naked) void WinmmFwd161() { __asm jmp dword ptr [g_winmm + 644] }
extern "C" __declspec(naked) void WinmmFwd162() { __asm jmp dword ptr [g_winmm + 648] }
extern "C" __declspec(naked) void WinmmFwd163() { __asm jmp dword ptr [g_winmm + 652] }
extern "C" __declspec(naked) void WinmmFwd164() { __asm jmp dword ptr [g_winmm + 656] }
extern "C" __declspec(naked) void WinmmFwd165() { __asm jmp dword ptr [g_winmm + 660] }
extern "C" __declspec(naked) void WinmmFwd166() { __asm jmp dword ptr [g_winmm + 664] }
extern "C" __declspec(naked) void WinmmFwd167() { __asm jmp dword ptr [g_winmm + 668] }
extern "C" __declspec(naked) void WinmmFwd168() { __asm jmp dword ptr [g_winmm + 672] }
extern "C" __declspec(naked) void WinmmFwd169() { __asm jmp dword ptr [g_winmm + 676] }
extern "C" __declspec(naked) void WinmmFwd170() { __asm jmp dword ptr [g_winmm + 680] }
extern "C" __declspec(naked) void WinmmFwd171() { __asm jmp dword ptr [g_winmm + 684] }
extern "C" __declspec(naked) void WinmmFwd172() { __asm jmp dword ptr [g_winmm + 688] }
extern "C" __declspec(naked) void WinmmFwd173() { __asm jmp dword ptr [g_winmm + 692] }
extern "C" __declspec(naked) void WinmmFwd174() { __asm jmp dword ptr [g_winmm + 696] }
extern "C" __declspec(naked) void WinmmFwd175() { __asm jmp dword ptr [g_winmm + 700] }
extern "C" __declspec(naked) void WinmmFwd176() { __asm jmp dword ptr [g_winmm + 704] }
extern "C" __declspec(naked) void WinmmFwd177() { __asm jmp dword ptr [g_winmm + 708] }
extern "C" __declspec(naked) void WinmmFwd178() { __asm jmp dword ptr [g_winmm + 712] }
extern "C" __declspec(naked) void WinmmFwd179() { __asm jmp dword ptr [g_winmm + 716] }
extern "C" __declspec(naked) void WinmmFwd180() { __asm jmp dword ptr [g_winmm + 720] }
extern "C" __declspec(naked) void WinmmFwd181() { __asm jmp dword ptr [g_winmm + 724] }
extern "C" __declspec(naked) void WinmmFwd182() { __asm jmp dword ptr [g_winmm + 728] }
extern "C" __declspec(naked) void WinmmFwd183() { __asm jmp dword ptr [g_winmm + 732] }
extern "C" __declspec(naked) void WinmmFwd184() { __asm jmp dword ptr [g_winmm + 736] }
extern "C" __declspec(naked) void WinmmFwd185() { __asm jmp dword ptr [g_winmm + 740] }
extern "C" __declspec(naked) void WinmmFwd186() { __asm jmp dword ptr [g_winmm + 744] }
extern "C" __declspec(naked) void WinmmFwd187() { __asm jmp dword ptr [g_winmm + 748] }
extern "C" __declspec(naked) void WinmmFwd188() { __asm jmp dword ptr [g_winmm + 752] }
extern "C" __declspec(naked) void WinmmFwd189() { __asm jmp dword ptr [g_winmm + 756] }
extern "C" __declspec(naked) void WinmmFwd190() { __asm jmp dword ptr [g_winmm + 760] }
extern "C" __declspec(naked) void WinmmFwd191() { __asm jmp dword ptr [g_winmm + 764] }
extern "C" __declspec(naked) void WinmmFwd192() { __asm jmp dword ptr [g_winmm + 768] }
