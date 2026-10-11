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
  libxkbcommon,
  pipewire,
  libGL,
  wireplumber,
  networkmanager,
  playerctl,
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
    libxkbcommon
    pipewire
    libGL
  ];

  doCheck = true;

  # The popup reads volume, network and media through these. They are
  # appended to PATH so the user's own versions win.
  postInstall = ''
    wrapProgram $out/bin/swell --suffix PATH : ${
      lib.makeBinPath [
        wireplumber
        networkmanager
        playerctl
      ]
    }
  '';

  meta = {
    description = "A small sway shell: OSD, hold-to-peek popup, workspace overview, dynamic workspaces and event hooks";
    license = lib.licenses.mit;
    platforms = lib.platforms.linux;
    mainProgram = "swell";
  };
}
