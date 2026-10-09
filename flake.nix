{
  description = "swell: a small sway shell (OSD, popup, overview, dynamic workspaces, event hooks)";

  inputs.nixpkgs.url = "github:nixos/nixpkgs?ref=nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAll = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAll (pkgs: {
        swell = pkgs.callPackage ./nix/package.nix { };
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.swell;
      });

      apps = forAll (pkgs: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.default}/bin/swell";
        };
      });

      devShells = forAll (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
          packages = with pkgs; [
            clang-tools
            gdb
            valgrind
            # live tests (test/live/nested-sway.sh)
            sway
            grim
          ];
        };
      });

      checks = forAll (pkgs: {
        swell = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      });

      overlays.default = final: prev: { swell = final.callPackage ./nix/package.nix { }; };

      homeManagerModules.default = import ./nix/hm-module.nix { inherit self; };
      homeModules.default = self.homeManagerModules.default;
    };
}
