' EOS M6 Mark II read-only identification script
' This script does NOT modify BOOTDISK or camera NVRAM.
' Adapted from the maintained Canon Basic Universal/extend_fw_sign.m example.
' EOS/DIGIC 8 Canon Basic uses B:/ for the SD card path.

dim pRom0BaseAddress = 0xE0040000
dim pRom1BaseAddress = 0xF0000000
dim sigLen = 0x10000

private sub compute_signature(startSign, sLen)
  p = startSign
  c = 0
  For i = 0 To (sLen - 1)
    c = c + *p
    p = p + 4
  Next
  compute_signature = c
end sub

private sub Initialize()
  fileName = "B:/M6II_INFO.TXT"
  RemoveFile(fileName)

  f = OpenFileCREAT(fileName)
  CloseFile(f)

  f = OpenFileWR(fileName)
  WriteFileString(f, "MODEL_ID=0x%08x\n", *pRom1BaseAddress)
  WriteFileString(f, "ROM_VERSION=%s\n", pRom1BaseAddress + 4)
  WriteFileString(f, "CANON_FW=%d\n", GetFirmwareVersion())
  WriteFileString(f, "ROM0_SIG_10000=0x%08x\n", compute_signature(pRom0BaseAddress, sigLen))
  CloseFile(f)
end sub
