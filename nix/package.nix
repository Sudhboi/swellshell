{
  lib,
  stdenv,
  meson,
  ninja,
  pkg-config,
  wayland-scanner,
  makeWrapper,
  wayland,
  wayland-protocols,
  cairo,
  pango,
  libjpeg,
  libxkbcommon,
  pipewire,
  libGL,
  libinput,
  udev,
  systemdLibs,
  wireplumber,
  networkmanager,
  playerctl,
  curl,
}:

stdenv.mkDerivation {
  pname = "swell";
  version = "0.1.0";

  src = lib.fileset.toSource {
    root = ./..;
    fileset = lib.fileset.unions [
      ../meson.build
      ../src
      ../lib
      ../protocols
      ../test
      ../config
    ];
  };

  strictDeps = true;
  depsBuildBuild = [ pkg-config ];
  nativeBuildInputs = [
    meson
    ninja
    pkg-config
    wayland-scanner
    makeWrapper
  ];
  buildInputs = [
    wayland
    wayland-protocols
    cairo
    pango
    libjpeg
    libxkbcommon
    pipewire
    libGL
    libinput
    udev
    systemdLibs # sd-bus: the screensaver inhibit service
  ];

  doCheck = true;

  # The popup reads volume, network and media through these (the wallpaper
  # media too, and curl fetches its covers). They are
  # appended to PATH so the user's own versions win.
  postInstall = ''
    wrapProgram $out/bin/swell --suffix PATH : ${
      lib.makeBinPath [
        wireplumber
        networkmanager
        playerctl
        curl
      ]
    }
  '';

  meta = {
    description = "A small Wayland shell for sway, niri and labwc: OSD, hold-to-peek popup, workspace overview, dynamic workspaces and event hooks";
    license = lib.licenses.gpl3Plus;
    platforms = lib.platforms.linux;
    mainProgram = "swell";
  };
}
