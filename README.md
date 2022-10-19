# PUISNE

## Portable UnINtruSive Executor

![Oh yea...](images/puisne.png)

## What is it?

A tool to package an executable & its resources quickly and easily. Think MacOS
[Application Bundles](https://developer.apple.com/library/archive/documentation/CoreFoundation/Conceptual/CFBundles/AboutBundles/AboutBundles.html#//apple_ref/doc/uid/10000123i-CH100-SW1) that
work anywhere, even across operating systems. They're exceedingly simple to work
with and to share.

See [the help text](cosmopolitan/puisne/help.txt) for usage instructions &
[mcdemigod.com/puisne](https://mcdemigod.com/puisne) for additional details.

## Make & run a package

A package is just a directory with a `.app` suffix. The directory should contain
an executable file with the same name, except the suffix. Simply zip the
directory into a `puisne.com` binary; you're free to change its filename to
whatever you want

```sh
mv puisne.com my_app.com
zip -r -D -g my_app.com my_app.app
```

You can now run the package exactly how you would the original executable:

```sh
./my_app.com argument to my_app
```

## Build from source

PUISNE is built with [Cosmopolitan Libc](https://github.com/jart/cosmopolitan),
(a fork of which is) included as a submodule of this repo. Pull the submodule &
descend therein to build with, eg.

```sh
git submodule init
git submodule update
cd cosmopolitan
make o//puisne
```
