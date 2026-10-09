{ self }:
{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.services.swell;
  toml = pkgs.formats.toml { };
in
{
  options.services.swell = {
    enable = lib.mkEnableOption "swell, a small sway shell";

    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "swell.packages.\${system}.default";
      description = "The swell package.";
    };

    settings = lib.mkOption {
      type = toml.type;
      default = { };
      example = lib.literalExpression ''
        {
          osd.hide_timeout_ms = 1500;
          overview.font = "Sans 11";
          runner.hook = [
            { on = "workspace.empty"; run = [ "awww unpause" ]; detach = true; }
            { on = "workspace.occupied"; run = [ "awww pause" ]; }
          ];
        }
      '';
      description = ''
        Written to {file}`$XDG_CONFIG_HOME/swell/config.toml`. Every key is
        optional; run `swell config --defaults` for the full list.
      '';
    };

    systemdTarget = lib.mkOption {
      type = lib.types.str;
      default = "graphical-session.target";
      example = "sway-session.target";
      description = "The systemd user target swell is started with.";
    };
  };

  config = lib.mkIf cfg.enable {
    home.packages = [ cfg.package ];

    xdg.configFile."swell/config.toml" = lib.mkIf (cfg.settings != { }) {
      source = toml.generate "swell-config.toml" cfg.settings;
      # Apply edits to a running daemon.
      onChange = "${lib.getExe cfg.package} reload >/dev/null 2>&1 || true";
    };

    systemd.user.services.swell = {
      Unit = {
        Description = "swell sway shell";
        PartOf = [ cfg.systemdTarget ];
        After = [ cfg.systemdTarget ];
        ConditionEnvironment = "WAYLAND_DISPLAY";
      };
      Service = {
        ExecStart = "${lib.getExe cfg.package} daemon";
        ExecReload = "${lib.getExe cfg.package} reload";
        Restart = "on-failure";
        RestartSec = 1;
      };
      Install.WantedBy = [ cfg.systemdTarget ];
    };
  };
}
