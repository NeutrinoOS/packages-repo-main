# BearSSL for Neutrino
## Building/updating
1. Acquire latest BearSSL from https://bearssl.org/gitweb/?p=BearSSL;a=summary
2. Copy the `src` folder from BearSSL into `src`
3. Copy the `inc` folder from BearSSL into `include/bearssl`
4. Bump version in `package/manifest.toml`
5. Run `make package` to build the library and package it
6. Congratulations, you now have a valid BearSSL neupak package