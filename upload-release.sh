#!/usr/bin/env bash

# Must be run in the source directory.
# Should have passed make distcheck.
# And all final changes should already have been pushed.
# Backup copy will be created in $HOME/debugedit-$VERSION

# Any error is fatal
set -e

# We take one arguent, the version (e.g. 0.2)
if [ $# -ne 1 ]; then
  echo "$0 <version> (e.g. 0.2)"
  exit 1
fi

VERSION="$1"

echo Make sure the git repo is tagged, signed and pushed
echo git tag -s -m \"debugedit $VERSION release\" debugedit-$VERSION
echo git push --tags

# Create a temporary directory and make sure it is cleaned up.
tempdir=$(mktemp -d) || exit
trap "rm -rf -- ${tempdir}" EXIT

pushd "${tempdir}"

# Checkout
git clone git://sourceware.org/git/debugedit.git
cd debugedit
git tag --verify "debugedit-${VERSION}"
git checkout -b "$VERSION" "debugedit-${VERSION}"

# Create dist
autoreconf -v -f -i
./configure
make -j$(nproc) && make distcheck

# Sign
mkdir $VERSION
cp debugedit-$VERSION.tar.xz $VERSION/
cd $VERSION/
gpg -b debugedit-$VERSION.tar.xz
cd ..

# Backup copy
cp -r $VERSION $HOME/debugedit-$VERSION

# Upload
scp -r $VERSION sourceware.org:/sourceware/ftp/pub/debugedit/
ssh sourceware.org "(cd /sourceware/ftp/pub/debugedit \
  && chmod go+rx $VERSION \
  && chmod go+r  $VERSION/debugedit-$VERSION.tar.xz* \
  && ln -sf $VERSION/debugedit-$VERSION.tar.xz \
	debugedit-latest.tar.xz \
  && ln -sf $VERSION/debugedit-$VERSION.tar.xz.sig \
	debugedit-latest.tar.xz.sig \
  && ls -lah debugedit-latest*)"

# Cleanup
popd
trap - EXIT
exit
