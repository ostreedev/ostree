---
nav_order: 105
---

# Pulling directly from Amazon S3
{: .no_toc }

1. TOC
{:toc}

<!-- SPDX-License-Identifier: (CC-BY-SA-3.0 OR GFDL-1.3-or-later) -->

A remote URL of the form `s3://bucket/prefix` pulls from a private S3 bucket
holding an archive-mode repository, using whatever AWS credentials the host
already has. Requests are signed with AWS Signature Version 4 and sent over
the normal curl-based fetcher, so existing remote options such as `proxy`
and `tls-ca-path` still apply.

Only pulling is supported; publishing to the bucket is done with other tools.

## Building

Configure with `--with-curl --with-aws`. This needs
[aws-c-auth](https://github.com/awslabs/aws-c-auth) 0.9.1 or later and the
AWS C libraries it depends on. They ship no pkg-config files; if they are not
on the default search path, set `AWS_CFLAGS` and `AWS_LIBS`. A build with
support lists `aws` under features in `ostree --version`; a build without it
rejects `s3://` URLs.

## URL syntax

```
s3://<bucket>/<prefix>[?region=<region>][&profile=<profile>][&endpoint=<url>]
```

- `region`: the bucket's region. If omitted, the first of `AWS_REGION`,
  `AWS_DEFAULT_REGION`, the profile's `region`, and the EC2 instance metadata
  service is used.
- `profile`: the shared-config profile to take credentials and region from.
  Defaults to `AWS_PROFILE`, then `default`.
- `endpoint`: `http(s)://host[:port]` of an S3-compatible server, such as
  MinIO, or a FIPS or VPC endpoint. Requests then use path-style addressing.
  An endpoint remote with no region signs for `us-east-1`.

Any other query parameter, a port, a fragment, or credentials in the URL are
rejected. Credentials always come from the AWS credential chain.

For example:

```
ostree remote add --set=gpg-verify=false os 's3://my-bucket/repo?region=us-gov-west-1'
ostree pull os main
```

## Credentials

Credentials are looked up in this order: environment variables, an IAM
Identity Center (SSO) profile, the shared config and credentials files
(static keys, `role_arn`, `credential_process`), web identity (EKS IRSA),
then ECS container credentials or EC2 instance metadata.

The identity needs `s3:GetObject` on the repository's keys **and**
`s3:ListBucket` on the bucket. Without `ListBucket`, S3 answers 403 rather
than 404 for keys that don't exist, and ostree, which probes for optional
files, treats a 403 as a hard error.

SSO sessions have a limitation: the underlying library reads the access
token that `aws sso login` caches but cannot refresh it, while the AWS CLI
can. Pulls therefore start failing when that token expires even though the
CLI still works. Either run `aws sso login` again, or give the profile
`credential_process = aws configure export-credentials --profile <p> --format process`,
which delegates to the CLI.

## Integrity

On a normal pull every object is checksummed before it is stored, so a
transfer problem can fail a pull but cannot store a wrong object.

Checksums only prove that objects match the commit that was fetched. To know
the commit itself is genuine, sign commits (`ostree sign`) and enable
`sign-verify` on the remote.

## Security notes

- Requests carry an `Authorization` header and, with temporary credentials,
  an `X-Amz-Security-Token` header. With `?endpoint=http://…` both travel
  unencrypted; use `https://` for anything but a local test server.
- `OSTREE_DEBUG_HTTP=1` prints request headers, including both of those.
- Anyone who can configure a remote can point signed requests at a host of
  their choosing with `?endpoint=`. Signatures are bound to that host, so
  they cannot be replayed against AWS, but the access key ID and any session
  token are disclosed to it. Treat permission to add or edit remotes
  accordingly.

## Limitations

- **Unverified trusted mirroring.** With `--http-trusted`, mirroring into an
  archive repo links fetched objects into the repository without verifying
  them, as it does for HTTP remotes, so a server that returns wrong bytes of
  the right length is not detected. Verifying each object before linking it,
  without recompressing it, would close this cheaply; it was left out
  because it is only needed for trusted mirroring and the same gap exists
  for HTTP remotes.
- Mirrorlists ignore `s3://` entries.
- The China partition (`amazonaws.com.cn`) is not supported.

## Testing

- `tests/test-fetcher-s3`: URL parsing and encoding, error parsing, signing
  against independently computed SigV4 signatures, region and credential
  resolution.
- `tests/test-pull-s3.sh`: pulls from a local MinIO with static and temporary
  credentials, static deltas, and error cases.
