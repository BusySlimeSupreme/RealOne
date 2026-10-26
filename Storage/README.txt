Storage - the only place the RealOne user ("RealUser", login name realuser) can live and write.
  Storage/User         home of the "realone" user (settings, layout, wallpapers)
  Storage/Data/Apps    app data (one folder per app) and user-installed .RAK apps
  Storage/Downloads    downloaded files (never executable)
  Storage/Temps        temporary files (TMPDIR), cleared at every boot
On the installed image this lives at /Root/Storage. The rest of the system is read-only to the user.
Nothing that runs lives here: the OS, its apps and wallpapers are in /usr/local (root-owned, read-only, immutable).
Files here can never be executed, and no code is ever loaded from Storage.
