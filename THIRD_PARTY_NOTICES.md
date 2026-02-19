# Third-Party Notices

`macKinect` uses external open-source projects and libraries.

## Core Kinect Libraries

1. OpenKinect `libfreenect`
Source: `https://github.com/OpenKinect/libfreenect`
Local source used during builds: `../libfreenect`
License files in local source tree: `APACHE20`, `GPL2`

2. OpenKinect `libfreenect2`
Source: `https://github.com/OpenKinect/libfreenect2`
Local source used during builds: `../libfreenect2`
License files in local source tree: `APACHE20`, `GPL2`

## Bundled Runtime Dependencies

When packaging the app/plugins, the build bundles dynamic libraries that are discovered from the local toolchain/environment, including:

- `libusb-1.0`
- `libturbojpeg`

If you redistribute packaged binaries, ensure your distribution includes all required notices and complies with each dependency license.

## Related Projects Referenced During Design

These repositories were referenced for architecture and feature direction (not bundled as direct source in this repo):

- `https://github.com/OpenKinect/`
- `https://github.com/SirLynix/obs-kinect`
- `https://github.com/MarekKowalski/LiveScan3D`
- `https://github.com/orgs/KinectToVR/repositories`
