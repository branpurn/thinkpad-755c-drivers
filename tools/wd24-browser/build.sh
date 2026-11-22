#!/bin/sh
# Static i486 nsfb. Pins are the commits and tarball hashes read on
# 2026-11-22. WD24X_NS_CHECK=1 prints the pins and does not compile.
set -eu
NS_SHA=170d89462442c286f80c0604f30e4c7f2a801265
NSFB_SHA=a287525eeafa4b9e721822670e1e03eeeef6b31c
TC_SHA512=0a6e508d919d2b5404e53c5bad7ea10924967c4b528012495ecc41ca3f4b371a0da4d814a2a5a18e079b1c330fa808ab0eaa924a3571f3792bcf9004feee856c
ZLIB_SHA=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
PNG_SHA=631a4c58ea6c10c81f160c4b21fa8495b715d251698ebc2552077e8450f30454
JPEG_SHA=04705c110cb2469caa79fb71fba3d7bf834914706e9641a4589485c1f832565b
ROOT=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
PREFIX=${WD24X_NS_PREFIX:-/tmp/wd24-ns-prefix}
SRC=${WD24X_NS_SRC:-/tmp/wd24-ns-src}
TC=${WD24X_NS_TC:-/tmp/src/i486-linux-musl-cross}
XRATE=1970

echo "WD24X-BROWSER pin netsurf $NS_SHA"
echo "WD24X-BROWSER pin libnsfb $NSFB_SHA"
echo "WD24X-BROWSER pin musl-cc i486-linux-musl-cross.tgz $TC_SHA512"
echo "WD24X-BROWSER pin zlib-1.3.1 $ZLIB_SHA"
echo "WD24X-BROWSER pin libpng-1.6.47 $PNG_SHA"
echo "WD24X-BROWSER pin jpeg-9f $JPEG_SHA"
if [ "${WD24X_NS_CHECK:-}" = 1 ]; then
	echo "WD24X-BROWSER patch $ROOT/patches/libnsfb-linux-fbdev.patch"
	echo "WD24X-BROWSER patch $ROOT/patches/netsurf-fb-dump.patch"
	echo "WD24X-BROWSER patch $ROOT/patches/netsurf-repro.patch"
	exit 0
fi

stop() {
	echo "WD24X-BROWSER stop $1" >&2
	exit 1
}

need() {
	command -v "$1" >/dev/null 2>&1 || stop "no $1"
}

# Host tools. The i486 compiler is downloaded below. libpng-dev is the
# host header convert_image links, not the cross libpng.
missing=
for t in git curl make gcc perl pkg-config gperf flex bison file readelf \
	sha256sum sha512sum tar qemu-i386-static; do
	command -v "$t" >/dev/null 2>&1 || missing="$missing $t"
done
[ -f /usr/include/png.h ] || missing="$missing libpng-dev"
[ -x /usr/bin/time ] || missing="$missing time"
[ -z "$missing" ] || stop "host deps:$missing"

fetch() {
	url=$1
	out=$2
	sum=$3
	if [ -f "$out" ]; then
		got=$(sha256sum "$out" | awk '{print $1}')
		[ "$got" = "$sum" ] || stop "checksum $out"
		return 0
	fi
	need curl
	curl -fL --retry 3 -o "$out.part" "$url" || stop "download $url"
	got=$(sha256sum "$out.part" | awk '{print $1}')
	[ "$got" = "$sum" ] || stop "checksum $url"
	mv "$out.part" "$out"
}

pin_git() {
	name=$1
	sha=$2
	dest=$SRC/$name
	if [ ! -d "$dest/.git" ]; then
		git clone --depth 1 https://github.com/netsurf-browser/"$name".git "$dest" \
			|| stop "clone $name"
	fi
	git -C "$dest" fetch --depth 1 origin "$sha" || stop "fetch $name"
	git -C "$dest" checkout --detach "$sha" || stop "checkout $name"
	git -C "$dest" reset --hard "$sha" || stop "reset $name"
	git -C "$dest" clean -fd >/dev/null
	got=$(git -C "$dest" rev-parse HEAD)
	[ "$got" = "$sha" ] || stop "pin $name $got"
}

if [ ! -x "$TC/bin/i486-linux-musl-gcc" ]; then
	need curl
	mkdir -p "$(dirname "$TC")"
	tgz=$TC.tgz
	if [ ! -f "$tgz" ]; then
		curl -fL --retry 3 -o "$tgz.part" \
			https://musl.cc/i486-linux-musl-cross.tgz \
			|| stop "download musl.cc"
		mv "$tgz.part" "$tgz"
	fi
	got=$(sha512sum "$tgz" | awk '{print $1}')
	[ "$got" = "$TC_SHA512" ] || stop "checksum musl.cc"
	tar -C "$(dirname "$TC")" -xzf "$tgz" || stop "unpack musl.cc"
fi
[ -x "$TC/bin/i486-linux-musl-gcc" ] || stop "no i486-linux-musl-gcc"
export PATH="$TC/bin:$PATH"
CC=$TC/bin/i486-linux-musl-gcc
AR=$TC/bin/i486-linux-musl-ar
RANLIB=$TC/bin/i486-linux-musl-ranlib
export CC AR RANLIB
# Fixed identity. The netsurf testament reads these instead of the
# build account, the hostname, and the clock. 1794996956 is the
# netsurf pin's commit time.
export HOSTNAME=tc32
export SOURCE_DATE_EPOCH=1794996956
export WD24X_BUILD_USER=tc32
export WD24X_BUILD_ROOT=/src/netsurf/
# Upstream library makefiles pass -Werror. GCC 11 warns on this tree,
# so -Wno-error is last and keeps the -O2 i486 objects building.
# Prefix maps keep this machine's source path out of __FILE__.
export CFLAGS="-O2 -march=i486 -mtune=i486 -fno-stack-protector -Wno-error -ffile-prefix-map=${SRC}=/src -ffile-prefix-map=${PREFIX}=/prefix -fdebug-prefix-map=${SRC}=/src -fdebug-prefix-map=${PREFIX}=/prefix -fmacro-prefix-map=${SRC}=/src"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
unset PKG_CONFIG_SYSROOT_DIR

mkdir -p "$SRC/tarballs" "$PREFIX"
fetch https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz \
	"$SRC/tarballs/zlib-1.3.1.tar.gz" "$ZLIB_SHA"
fetch https://github.com/pnggroup/libpng/archive/refs/tags/v1.6.47.tar.gz \
	"$SRC/tarballs/libpng-1.6.47.tar.gz" "$PNG_SHA"
fetch https://www.ijg.org/files/jpegsrc.v9f.tar.gz \
	"$SRC/tarballs/jpegsrc.v9f.tar.gz" "$JPEG_SHA"

if [ ! -f "$PREFIX/lib/libz.a" ]; then
	rm -rf "$SRC/zlib-1.3.1"
	tar -C "$SRC" -xzf "$SRC/tarballs/zlib-1.3.1.tar.gz"
	(cd "$SRC/zlib-1.3.1" && ./configure --prefix="$PREFIX" --static) \
		|| stop "zlib configure"
	make -C "$SRC/zlib-1.3.1" -j2 || stop "zlib"
	make -C "$SRC/zlib-1.3.1" install || stop "zlib install"
fi

if [ ! -f "$PREFIX/lib/libpng.a" ] && [ ! -f "$PREFIX/lib/libpng16.a" ]; then
	rm -rf "$SRC/libpng-1.6.47"
	tar -C "$SRC" -xzf "$SRC/tarballs/libpng-1.6.47.tar.gz"
	(cd "$SRC/libpng-1.6.47" && \
		CPPFLAGS="-I$PREFIX/include" LDFLAGS="-L$PREFIX/lib" \
		./configure --host=i486-linux-musl --prefix="$PREFIX" \
			--enable-static --disable-shared) || stop "libpng configure"
	make -C "$SRC/libpng-1.6.47" -j2 || stop "libpng"
	make -C "$SRC/libpng-1.6.47" install || stop "libpng install"
fi

if [ ! -f "$PREFIX/lib/libjpeg.a" ]; then
	rm -rf "$SRC/jpeg-9f"
	tar -C "$SRC" -xzf "$SRC/tarballs/jpegsrc.v9f.tar.gz"
	(cd "$SRC/jpeg-9f" && \
		./configure --host=i486-linux-musl --prefix="$PREFIX" \
			--enable-static --disable-shared) || stop "jpeg configure"
	make -C "$SRC/jpeg-9f" -j2 || stop "jpeg"
	make -C "$SRC/jpeg-9f" install || stop "jpeg install"
	mkdir -p "$PREFIX/lib/pkgconfig"
	cat > "$PREFIX/lib/pkgconfig/libjpeg.pc" << EOF
prefix=$PREFIX
Name: libjpeg
Description: IJG JPEG library
Version: 9f
Libs: -L\${prefix}/lib -ljpeg
Cflags: -I\${prefix}/include
EOF
fi

pin_git buildsystem b38fe7146aa3af08881ba9d6817f19ee7abe4a86
make -C "$SRC/buildsystem" install PREFIX="$PREFIX" || stop "buildsystem"

install_lib() {
	name=$1
	sha=$2
	shift 2
	pin_git "$name" "$sha"
	# libdom's XML binding wants expat. The file page does not.
	make -C "$SRC/$name" -j2 install PREFIX="$PREFIX" Q= \
		CC="$CC" AR="$AR" RANLIB="$RANLIB" \
		"$@" \
		|| stop "build $name"
}

install_lib libwapcaplet 74e49210e9c56381c1e60aea309dc80074a54ede
install_lib libparserutils c9971265064b194505c6cd398dbdef8788725de7
install_lib libhubbub 68989e60c786f1ee490db95c44769c368d135262
install_lib libdom e4150b65e4e77d6e028912c5cdae084b3a564c93 WITH_EXPAT_BINDING=no
install_lib libcss bc7332b40bb8d9fd6ce5249a7a8abb58d30ab824
install_lib libnsgif b9e3b0aadce374cce45c34feb55ad73a3381ff98
install_lib libnsbmp 3406e7e0bfb5a8edc73af1cd4ae39553907514fb
install_lib libnsutils 4f0b88241304d97e10c363b81f3cf959404da82b
install_lib libnspsl fb372b56cc83b81143c677fee526c91c399596ae
install_lib libnslog 6c3445f5e78cc13f4f524b66a4092936e77ef534

pin_git libnsfb "$NSFB_SHA"
git -C "$SRC/libnsfb" apply "$ROOT/patches/libnsfb-linux-fbdev.patch" \
	|| stop "libnsfb patch"
install_lib_built() {
	make -C "$SRC/libnsfb" install PREFIX="$PREFIX" Q= \
		CC="$CC" AR="$AR" RANLIB="$RANLIB" \
		|| stop "build libnsfb"
}
install_lib_built

pin_git netsurf "$NS_SHA"
git -C "$SRC/netsurf" apply "$ROOT/patches/netsurf-fb-dump.patch" \
	|| stop "netsurf patch"
git -C "$SRC/netsurf" apply "$ROOT/patches/netsurf-repro.patch" \
	|| stop "netsurf repro patch"
cat > "$SRC/netsurf/Makefile.config" << 'EOF'
override NETSURF_USE_CURL := NO
override NETSURF_USE_OPENSSL := NO
override NETSURF_USE_BMP := YES
override NETSURF_USE_GIF := YES
override NETSURF_USE_JPEG := YES
override NETSURF_USE_JPEGXL := NO
override NETSURF_USE_PNG := YES
override NETSURF_USE_VIDEO := NO
override NETSURF_USE_WEBP := NO
override NETSURF_USE_DUKTAPE := NO
override NETSURF_USE_HARU_PDF := NO
override NETSURF_USE_NSSVG := NO
override NETSURF_USE_ROSPRITE := NO
override NETSURF_USE_NSPSL := YES
override NETSURF_USE_NSLOG := YES
override NETSURF_USE_LIBICONV_PLUG := YES
override NETSURF_USE_UTF8PROC := NO
override NETSURF_FB_FONTLIB := internal
override NETSURF_STRIP_BINARY := NO
# No build-prefix path. The operator sets NETSURFRES.
override NETSURF_FB_RESPATH := $${HOME}/.netsurf/:$${NETSURFRES}
override NETSURF_FB_FONTPATH :=
EOF
export LDFLAGS="-L$PREFIX/lib -static"
make -C "$SRC/netsurf" TARGET=framebuffer BUILD_CC=gcc CCACHE= \
	PREFIX="$PREFIX" Q= \
	|| stop "nsfb"
BIN=$SRC/netsurf/nsfb
[ -f "$BIN" ] || BIN=$SRC/netsurf/build-framebuffer/nsfb
[ -f "$BIN" ] || stop "no nsfb binary"
mkdir -p "$PREFIX/bin"
cp "$BIN" "$PREFIX/bin/nsfb"
i486-linux-musl-strip -o "$PREFIX/bin/nsfb.strip" "$PREFIX/bin/nsfb" \
	|| stop "strip"
mach=$(readelf -h "$PREFIX/bin/nsfb.strip" | awk '/Machine:/ {print}')
echo "$mach" | grep -q 'Intel 80386' || stop "not i386 ($mach)"
if readelf -l "$PREFIX/bin/nsfb.strip" | grep -q 'INTERP'; then
	stop "not static"
fi

bytes=$(wc -c < "$PREFIX/bin/nsfb.strip" | tr -d ' ')
sum=$(sha256sum "$PREFIX/bin/nsfb.strip" | awk '{print $1}')
whole=$((bytes / XRATE))
rem=$((bytes % XRATE))
if [ "$rem" -eq 0 ]; then
	secs=$whole
else
	secs=$((whole + 1))
fi
stage=$PREFIX/nsfb
rm -rf "$stage"
mkdir -p "$stage/nsres/en" "$stage/pages"
cp "$PREFIX/bin/nsfb.strip" "$stage/nsfb"
res=$SRC/netsurf/frontends/framebuffer/res
for f in Messages default.css internal.css quirks.css adblock.css \
	welcome.html licence.html credits.html favicon.png netsurf.png; do
	cp "$res/$f" "$stage/nsres/$f"
done
cp -a "$res/en/." "$stage/nsres/en/"
cp -a "$res/icons" "$res/pointers" "$res/throbber" "$res/fonts" "$stage/nsres/"
cp -a "$ROOT/pages/." "$stage/pages/"
echo "WD24X-BROWSER stage $stage"
echo "WD24X-BROWSER binary $stage/nsfb $bytes $sum"
echo "WD24X-BROWSER xmodem $secs at $XRATE remainder $rem"
# Stripped static binary from the fixed identity above.
expect=${WD24X_NS_EXPECT_SHA:-531803c04944918491480e4add46641c5a91ee0d1dc8841fab81cc1098c7f446}
if [ "$sum" != "$expect" ]; then
	stop "sha $sum"
fi

if [ "${WD24X_NS_SMOKE:-1}" = 0 ]; then
	echo "WD24X-BROWSER done"
	exit 0
fi
need qemu-i386-static
page=$stage/pages/canvas.html
dump=$SRC/fb.raw
rm -f "$dump"
# Ram, not linux: this host has no /dev/fb0. NETSURFRES is the English
# resource set the 755C launch uses. Without Messages the page stays white.
set +e
WD24X_FB_DUMP=$dump NETSURFRES=$stage/nsres/ /usr/bin/time -v \
	qemu-i386-static -cpu 486 \
	"$stage/nsfb" \
	-f ram -b 8 -w 640 -h 480 "file://$page" \
	> "$SRC/smoke.out" 2> "$SRC/smoke.err"
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
	echo "WD24X-BROWSER stop qemu rc $rc" >&2
	tail -40 "$SRC/smoke.err" >&2 || true
	exit 1
fi
grep -q 'WD24X-BROWSER dump' "$SRC/smoke.err" || stop "no dump"
rss=$(awk '/Maximum resident set size/{print $6}' "$SRC/smoke.err" | tail -1)
[ -n "$rss" ] || stop "no rss"
fbbytes=$(wc -c < "$dump" | tr -d ' ')
fbsum=$(sha256sum "$dump" | awk '{print $1}')
echo "WD24X-BROWSER fb $fbsum $fbbytes"
echo "WD24X-BROWSER rss $rss kB qemu-user"
echo "WD24X-BROWSER done"
