' EOS M6 Mark II BOOTDISK disabler / recovery
' This reverses the persistent BOOTDISK camera flag set by enable/extend.m.

private sub Initialize()
  System.Create()
  DisableBootDisk()

  fileName = "A:/BOOTDISK_DISABLED.TXT"
  RemoveFile(fileName)
  f = OpenFileCREAT(fileName)
  CloseFile(f)
  f = OpenFileWR(fileName)
  WriteFileString(f, "EOS M6 Mark II: DisableBootDisk() was called.\n")
  CloseFile(f)
end sub
