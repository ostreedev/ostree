---
nav_order: 100
---

# Handling access to authenticated remote repositories
{: .no_toc }

1. TOC
{:toc}

<!-- SPDX-License-Identifier: (CC-BY-SA-3.0 OR GFDL-1.3-or-later) -->

There is no default concept of an "ostree server"; ostree expects to talk to a generic webserver, so any tool and technique applicable for generic HTTP can also apply to fetching content via OSTree's builtin HTTP client.

## Using mutual TLS

The `tls-client-cert-path` and `tls-client-key-path` expose the underlying HTTP code for [mutual TLS](https://en.wikipedia.org/wiki/Mutual_authentication).

Each device can be provisioned with a secret key which grants it access to the webserver.

## Using basic authentication

The client supports HTTP `basic` authentication, but this has well-known management drawbacks.

## Using cookies

Since [this pull request](https://github.com/ostreedev/ostree/pull/531) ostree supports adding cookies to a remote configuration.  This can be used with e.g. [Amazon CloudFront](https://docs.aws.amazon.com/AmazonCloudFront/latest/DeveloperGuide/private-content-signed-cookies.html).
