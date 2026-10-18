# ReToon images

The system images of ReToon, one release per version. The ReToon setup app
([ToonSoftwareCollective/ReToon](https://github.com/ToonSoftwareCollective/ReToon))
downloads the one for its Toon from here and checks every file against the
checksums that come with the app.

Each release has:

- `retoon.tar.gz`: ReToon for the Toon 2 (kernel, device tree, initrd,
  system image, boot script).
- `retoon-toon1.tar.gz`: ReToon for the Toon 1 (kernel with ReToon's
  start-up built in, system image).

Each archive also holds `files.sha256` and `version.txt`.

The images are kept here and not in the app, because the app is installed
as a whole on a Toon, and a Toon 1 has no room for both images.
