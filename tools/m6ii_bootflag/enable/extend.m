' EOS M6 Mark II BOOTDISK enabler
' WARNING: this makes a persistent camera setting change.
' Run the verify script first and continue only on the supported M6II.111 / 5.9.2 target.
' Based on lclevy/cbasic_examples Universal/extend_bootdisk.m.

private sub Initialize()
  System.Create()
  EnableBootDisk()

  fileName = "A:/BOOTDISK_ENABLED.TXT"
  RemoveFile(fileName)
  f = OpenFileCREAT(fileName)
  CloseFile(f)
  f = OpenFileWR(fileName)
  WriteFileString(f, "EOS M6 Mark II: EnableBootDisk() was called.\n")
  CloseFile(f)
end sub
