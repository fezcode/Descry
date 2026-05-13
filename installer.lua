local version = "0.67.0"

SetMetadata("AppName", "Downsee")
SetMetadata("Version", version)
SetMetadata("Company", "Fezcode")

SetTheme("Windows11")
SetInstallDirSuffix("Downsee")

AddStep("Welcome", { title = "Downsee " .. version, description = "This wizard will install Downsee " .. version .. " on your computer.\n\nClick Next to continue." })
AddStep("Folder", { title = "Select Folder", description = "Choose where to install Downsee " .. version .. "." })
AddStep("Shortcuts", {title = "Select Optional Tasks", description = "Check the items you would like the installer to perform." })
AddStep("Install", { title = "Installing...", description = "Copying files to your system." })
AddStep("Finish", { title = "Installation Complete", description = "Downsee has been installed successfully.\n\nClick Finish to close.\n" })

MkDir("%INSTALLDIR%")

CopyFiles("build/downsee.exe",            "%INSTALLDIR%/downsee.exe")
CopyFiles("build/SDL2.dll",               "%INSTALLDIR%/SDL2.dll")
CopyFiles("build/libbrotlicommon.dll",    "%INSTALLDIR%/libbrotlicommon.dll")
CopyFiles("build/libbrotlidec.dll",       "%INSTALLDIR%/libbrotlidec.dll")
CopyFiles("build/libbz2-1.dll",           "%INSTALLDIR%/libbz2-1.dll")
CopyFiles("build/libfreetype-6.dll",      "%INSTALLDIR%/libfreetype-6.dll")
CopyFiles("build/libgcc_s_seh-1.dll",     "%INSTALLDIR%/libgcc_s_seh-1.dll")
CopyFiles("build/libglib-2.0-0.dll",      "%INSTALLDIR%/libglib-2.0-0.dll")
CopyFiles("build/libgraphite2.dll",       "%INSTALLDIR%/libgraphite2.dll")
CopyFiles("build/libharfbuzz-0.dll",      "%INSTALLDIR%/libharfbuzz-0.dll")
CopyFiles("build/libiconv-2.dll",         "%INSTALLDIR%/libiconv-2.dll")
CopyFiles("build/libintl-8.dll",          "%INSTALLDIR%/libintl-8.dll")
CopyFiles("build/libjpeg-8.dll",          "%INSTALLDIR%/libjpeg-8.dll")
CopyFiles("build/libpcre2-8-0.dll",       "%INSTALLDIR%/libpcre2-8-0.dll")
CopyFiles("build/libpng16-16.dll",        "%INSTALLDIR%/libpng16-16.dll")
CopyFiles("build/libstdc++-6.dll",        "%INSTALLDIR%/libstdc++-6.dll")
CopyFiles("build/libwinpthread-1.dll",    "%INSTALLDIR%/libwinpthread-1.dll")
CopyFiles("build/zlib1.dll",              "%INSTALLDIR%/zlib1.dll")

CopyFiles("README.md", "%INSTALLDIR%/README.md")

-- Desktop and Start Menu shortcuts
CreateShortcut("%INSTALLDIR%/downsee.exe", "%DESKTOP%", "Downsee", { label = "Create Desktop Shortcut", isOptional = true, isSelected = true })
CreateShortcut("%INSTALLDIR%/downsee.exe", "%STARTMENU%/Fezcode", "Downsee", { label = "Create Start Menu Entry", isOptional = true, isSelected = true })

CheckRegistry("HKCU", "Software\\Fezcode\\Downsee", "InstallDir")
CreateRegistry("HKCU", "Software\\Fezcode\\Downsee", "InstallDir", "%INSTALLDIR%")
CreateRegistry("HKCU", "Software\\Fezcode\\Downsee", "Version", version)
