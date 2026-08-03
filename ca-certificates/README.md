# CA certificates for Neutrino

This data-only package installs Neutrino's shared TLS trust store at
`/config/ssl/cacert.pem`. TLS clients should depend on `ca-certificates`
instead of embedding their own copy of the bundle.

The bundle is generated from Mozilla's root certificate program and is
distributed by curl at <https://curl.se/docs/caextract.html>. Its header records
the source date and licensing information. Review that metadata whenever the
bundle is updated, then bump this package's version.

Build the uncompressed neupak archive with:

```sh
make package
```

The archive is written to `out/ca-certificates.zip`.
