{
  stdenv,
  lib,
  mkMesonExecutable,

  nix-store,
  nix-expr,
  nix-main,
  nix-cmd,

  # Configuration Options

  version,
}:

let
  inherit (lib) fileset;
in

mkMesonExecutable (finalAttrs: {
  pname = "nix";
  inherit version;

  workDir = ./.;
  fileset = fileset.unions (
    [
      ../../nix-meson-build-support
      ./nix-meson-build-support
      ../../.version
      ./.version
      ./meson.build
      ./meson.options

      # Symbolic links to other dirs
      ## exes
      ./build-remote
      ./doc
      ./nix-build
      ./nix-channel
      ./nix-collect-garbage
      ./nix-copy-closure
      ./nix-env
      ./nix-instantiate
      ./nix-store
      ## dirs
      ./scripts
      ../../scripts
      ./misc
      ../../misc

      # Doc nix files for --help
      ../../doc/manual/generate-manpage.nix
      ../../doc/manual/utils.nix
      ../../doc/manual/generate-settings.nix
      ../../doc/manual/generate-store-info.nix

      # Other files to be included as string literals
      ../nix-channel/unpack-channel.nix
      ../nix-env/buildenv.nix
      ./get-env.sh
      ./help-stores.md
      ../../doc/manual/source/store/types/index.md.in
      ./profiles.md
      ../../doc/manual/source/command-ref/files/profiles.md

      # Files
    ]
    ++
      lib.concatMap
        (dir: [
          (fileset.fileFilter (file: file.hasExt "cc") dir)
          (fileset.fileFilter (file: file.hasExt "hh") dir)
          (fileset.fileFilter (file: file.hasExt "md") dir)
        ])
        [
          ./.
          ../build-remote
          ../nix-build
          ../nix-channel
          ../nix-collect-garbage
          ../nix-copy-closure
          ../nix-env
          ../nix-instantiate
          ../nix-store
        ]
  );

  buildInputs = [
    nix-store
    nix-expr
    nix-main
    nix-cmd
  ];

  mesonFlags = [
  ];

  postInstall = lib.optionalString stdenv.hostPlatform.isStatic ''
    mkdir -p $out/nix-support
    echo "file binary-dist $out/bin/nix" >> $out/nix-support/hydra-build-products
  '';

  meta = {
    mainProgram = "nix";
    platforms = lib.platforms.unix ++ lib.platforms.windows;
  };

})
