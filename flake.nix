{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    flake-parts.url = "github:hercules-ci/flake-parts";
    flake-parts.inputs.nixpkgs-lib.follows = "nixpkgs";
  };

  outputs = inputs @ {flake-parts, ...}:
    flake-parts.lib.mkFlake {inherit inputs;} {
      imports = [];
      systems = ["aarch64-darwin" "aarch64-linux" "riscv64-linux" "x86_64-darwin" "x86_64-linux"];
      perSystem = {
        config,
        self',
        inputs',
        pkgs,
        system,
        ...
      }: {
        packages.default = pkgs.stdenv.mkDerivation {
          name = "txproto";
          src = ./.;

          nativeBuildInputs = with pkgs; [
            pkg-config
            meson
            ninja
            cmake
            python3
          ];

          buildInputs = with pkgs; [
            zlib
            libplacebo
            ffmpeg-full
            lua5_4_compat

            wayland
            wayland-scanner
            kdePackages.wayland-protocols
            libxkbcommon
            xorg.libxcb
            libdrm

            vulkan-headers
            vulkan-loader

            libedit

            libpulseaudio
          ];
          prePatch = ''
            patchShebangs .
          '';
          meta = with pkgs.lib; {
            homepage = "https://github.com/cyanreg/txproto";
            license = with licenses; [lgpl21];
          };
        };
        formatter = pkgs.alejandra;
      };
    };
}
