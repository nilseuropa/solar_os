+++
id = "ssh_keys"
title = "SSH identity keys"
section = "network"
summary = "Inspect, share, generate, and remove the default SSH key pair"
aliases = ["sshkeys", "sshkey"]
keywords = "python lua ssh scp keys identity private public generate remove credentials"
packages_any = ["service_ssh"]
+++
# SSH identity keys

SolarOS can keep a default SSH key pair for `ssh` and `scp`. The public key may
be copied to remote hosts; the private key must remain protected.

## Inspect or create a key

```text
sshkey status
sshkey gen 3072
sshkey pub
```

RSA keys default to 3072 bits. Explicit sizes of 2048, 3072 and 4096 bits are
supported; GitHub requires at least 3072 bits for newly added RSA keys.
Generating a key may take time and memory. Without `-f`, an existing key is not
overwritten. To replace a pair, preserve a backup first, then run
`sshkey gen -f 3072` and register the new public key with the remote hosts you use.

## Protect private material

Copy only the public key to the remote host's `authorized_keys`. Keep the
private key on the device. The agent's storage tools cannot access `.ssh`.

## Quick reference

solaros.ssh_keys provides default_paths, default_exists, status, public_key,
generate (optional bits and overwrite), and remove when SSH is installed.
`public_key()` returns only the default OpenSSH public-key line. Share that
line with the remote host; keep the private key on the device.
