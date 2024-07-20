#!/bin/bash
# Setup development environment for RPM based systems such as Red Hat, Centos, Fedora, openSUSE
#
# Wireshark - Network traffic analyzer
# By Gerald Combs <gerald@wireshark.org>
# Copyright 1998 Gerald Combs
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# We drag in tools that might not be needed by all users; it's easier
# that way.
#

set -e -u -o pipefail

function print_usage() {
	printf "\nUtility to setup a rpm-based system for Wireshark Development.\n"
	printf "The basic usage installs the needed software\n\n"
	printf "Usage: %s [--install-optional] [...other options...]\n" "$0"
	printf "\t--install-optional: install optional software as well\n"
	printf "\t--install-rpm-deps: install packages required to build the .rpm file\n"
	printf "\\t--install-qt5-deps: force installation of packages required to use Qt5\\n"
	printf "\\t--install-qt6-deps: force installation of packages required to use Qt6\\n"
	printf "\\t--install-all: install everything\\n"
	printf "\t[other]: other options are passed as-is to the package manager\n"
}

ADDITIONAL=0
RPMDEPS=0
ADD_QT5=0
ADD_QT6=0
HAVE_ADD_QT=0
OPTIONS=
for arg; do
	case $arg in
		--help|-h)
			print_usage
			exit 0
			;;
		--install-optional)
			ADDITIONAL=1
			;;
		--install-rpm-deps)
			RPMDEPS=1
			;;
		--install-qt5-deps)
			ADD_QT5=1
			HAVE_ADD_QT=1
			;;
		--install-qt6-deps)
			ADD_QT6=1
			HAVE_ADD_QT=1
			;;
		--install-all)
			ADDITIONAL=1
			RPMDEPS=1
			ADD_QT5=1
			ADD_QT6=1
			HAVE_ADD_QT=1
			;;
		*)
			OPTIONS="$OPTIONS $arg"
			;;
	esac
done

# Check if the user is root
if [ "$(id -u)" -ne 0 ]
then
	echo "You must be root."
	exit 1
fi

BASIC_LIST="
	cmake
	desktop-file-utils
	flex
	gcc
	gcc-c++
	git
	glib2-devel
	libgcrypt-devel
	libpcap-devel
	pcre2-devel
	python3
	zlib-devel
	"

ADDITIONAL_LIST="
	krb5-devel
	libcap-devel
	libssh-devel
	libxml2-devel
	lz4
	minizip-devel
	perl
	perl-Parse-Yapp
	python3-pytest
	python3-pytest-xdist
	snappy-devel
	spandsp-devel
	systemd-devel
	"

# Uncomment to add PNG compression utilities used by compress-pngs:
# ADDITIONAL_LIST="$ADDITIONAL_LIST
#	advancecomp
#	optipng
#	oxipng
#	pngcrush"

# XXX
RPMDEPS_LIST="rpm-build"

# Guess which package manager we will use
for PM in zypper dnf yum ''; do
	if type "$PM" >/dev/null 2>&1; then
		break
	fi
done

if [ -z "$PM" ]
then
	echo "No package managers found, exiting"
	exit 1
fi

PM_OPT=
case $PM in
	zypper)
		PM_OPT="--non-interactive"
		PM_SEARCH="search -x --provides"
		;;
	dnf)
		PM_SEARCH="info"
		;;
	yum)
		PM_SEARCH="info"
		;;
esac

echo "Using $PM ($PM_SEARCH)"

# Adds package $2 to list variable $1 if the package is found
add_package() {
	local list="$1" pkgname="$2"

	# fail if the package is not known
	# shellcheck disable=SC2086
	$PM $PM_SEARCH "$pkgname" &> /dev/null || return 1

	# package is found, append it to list
	eval "${list}=\"\${${list}} \${pkgname}\""
}

# Adds packages $2-$n to list variable $1 if all the packages are found
add_packages() {
	local list="$1" pkgnames="${*:2}"

	# fail if any package is not known
	for pkgname in $pkgnames; do
		# shellcheck disable=SC2086
		$PM $PM_SEARCH "$pkgname" &> /dev/null || return 1
	done

	# all packages are found, append it to list
	eval "${list}=\"\${${list}} \${pkgnames}\""
}

add_package BASIC_LIST glib2 || add_package BASIC_LIST libglib-2_0-0 ||
echo "Required package glib2|libglib-2_0-0 is unavailable" >&2

add_package BASIC_LIST lua53-devel || add_package BASIC_LIST lua-devel ||
echo "Required package lua53-devel|lua-devel is unavailable" >&2

add_package BASIC_LIST lua53 || add_package BASIC_LIST lua ||
echo "Required package lua53|lua is unavailable" >&2

add_package BASIC_LIST libpcap || add_package BASIC_LIST libpcap1 ||
echo "Required package libpcap|libpcap1 is unavailable" >&2

add_package BASIC_LIST zlib || add_package BASIC_LIST libz1 ||
echo "Required package zlib|libz1 is unavailable" >&2

add_package BASIC_LIST c-ares-devel || add_package BASIC_LIST libcares-devel ||
echo "Required package c-ares-devel|libcares-devel is unavailable" >&2

add_package BASIC_LIST speexdsp-devel || add_package BASIC_LIST speex-devel ||
echo "Required package speexdsp-devel|speex-devel is unavailable" >&2

if [ $HAVE_ADD_QT -eq 0 ]
then
	# Try to select Qt version from distro
	test -e /etc/os-release && os_release='/etc/os-release' || os_release='/usr/lib/os-release'
	# shellcheck disable=SC1090
	. "${os_release}"

	# Fedora 35 or later
	if [ "${ID:-linux}" = "fedora" ] && [ "${VERSION_ID:-0}" -ge "35" ]; then
		echo "Installing Qt6."
		ADD_QT6=1
	else
		echo "Installing Qt5."
		ADD_QT5=1
	fi
fi

if [ $ADD_QT5 -ne 0 ]
then
	# qt5-linguist: CentOS, Fedora
	# libqt5-linguist-devel: OpenSUSE
	add_package BASIC_LIST qt5-linguist ||
	add_package BASIC_LIST libqt5-linguist-devel ||
	echo "Required package qt5-linguist|libqt5-linguist-devel is unavailable" >&2

	# qt5-qtmultimedia: CentOS, Fedora, pulls in qt5-qtbase-devel (big dependency list!)
	# libqt5-qtmultimedia-devel: OpenSUSE, pulls in Core, Gui, Multimedia, Network, Widgets
	# OpenSUSE additionally has a separate Qt5PrintSupport package.
	add_package BASIC_LIST qt5-qtmultimedia-devel ||
	add_packages BASIC_LIST libqt5-qtmultimedia-devel libQt5PrintSupport-devel ||
	echo "Required Qt5 Multimedia and/or Qt5 Print Support is unavailable" >&2

	# This is only required on OpenSUSE
	add_package BASIC_LIST libqt5-qtsvg-devel ||
	echo "Required OpenSUSE package libqt5-qtsvg-devel is unavailable. Not required for other distributions." >&2

	# This is only required on OpenSUSE
	add_package BASIC_LIST libQt5Concurrent-devel ||
	echo "Required OpenSUSE package libQt5Concurrent-devel is unavailable. Not required for other distributions." >&2

	# This is only required on OpenSUSE
	add_package ADDITIONAL_LIST libQt5DBus-devel ||
	echo "Optional OpenSUSE package libQt5DBus-devel is unavailable. Not required for other distributions." >&2

	add_package ADDITIONAL_LIST qt5-qtimageformats ||
	add_package ADDITIONAL_LIST libqt5-qtimageformats ||
	echo "Optional Qt5 Image Formats is unavailable" >&2
fi

if [ $ADD_QT6 -ne 0 ]
then
	# Fedora Qt6 packages required from a minimal installation
	QT6_LIST=(qt6-qtbase-devel
			qt6-qttools-devel
			qt6-qt5compat-devel
			qt6-qtmultimedia-devel
			libxkbcommon-devel)

	for pkg in "${QT6_LIST[@]}"
	do
		add_package BASIC_LIST "$pkg" ||
		echo "Qt6 dependency $pkg is unavailable" >&2
	done

	add_package ADDITIONAL_LIST qt6-qtimageformats ||
	echo "Optional Qt6 Image Formats is unavailable" >&2
fi

# This in only required on OpenSUSE
add_packages BASIC_LIST hicolor-icon-theme xdg-utils ||
echo "Required OpenSUSE packages hicolor-icon-theme and xdg-utils are unavailable. Not required for other distributions." >&2

# This in only required (and available) on OpenSUSE
add_package BASIC_LIST update-desktop-files ||
echo "Required OpenSUSE package update-desktop-files is unavailable. Not required for other distributions." >&2

# rubygem-asciidoctor.noarch: Centos, Fedora
# (Added to RHEL/Centos 8: https://bugzilla.redhat.com/show_bug.cgi?id=1820896 )
# ruby2.5-rubygem-asciidoctor: openSUSE 15.2
add_package RPMDEPS_LIST rubygem-asciidoctor.noarch || add_package RPMDEPS_LIST ruby2.5-rubygem-asciidoctor ||
echo "RPM dependency asciidoctor is unavailable" >&2

# libcap: CentOS 7, Fedora 28, Fedora 29
# libcap2: OpenSUSE Leap 42.3, OpenSUSE Leap 15.0
add_package ADDITIONAL_LIST libcap || add_package ADDITIONAL_LIST libcap2 ||
echo "Optional package libcap|libcap2 is unavailable" >&2

add_package ADDITIONAL_LIST nghttp2-devel || add_package ADDITIONAL_LIST libnghttp2-devel ||
echo "Optional package nghttp2-devel|libnghttp2-devel is unavailable" >&2

add_package ADDITIONAL_LIST snappy || add_package ADDITIONAL_LIST libsnappy1 ||
echo "Optional package snappy|libsnappy1 is unavailable" >&2

add_package ADDITIONAL_LIST libzstd-devel || echo "Optional package lbzstd-devel is unavailable" >&2

add_package ADDITIONAL_LIST lz4-devel || add_package ADDITIONAL_LIST liblz4-devel ||
echo "Optional package lz4-devel|liblz4-devel is unavailable" >&2

add_package ADDITIONAL_LIST libcap-progs || echo "Optional package libcap-progs is unavailable" >&2

add_package ADDITIONAL_LIST libmaxminddb-devel ||
echo "Optional package libmaxminddb-devel is unavailable" >&2

add_package ADDITIONAL_LIST gnutls-devel || add_package ADDITIONAL_LIST libgnutls-devel ||
echo "Optional package gnutls-devel|libgnutls-devel is unavailable" >&2

add_package ADDITIONAL_LIST gettext-devel || add_package ADDITIONAL_LIST gettext-tools ||
echo "Optional package gettext-devel|gettext-tools is unavailable" >&2

add_package ADDITIONAL_LIST ninja || add_package ADDITIONAL_LIST ninja-build ||
echo "Optional package ninja|ninja-build is unavailable" >&2

add_package ADDITIONAL_LIST libxslt || add_package ADDITIONAL_LIST libxslt1 ||
echo "Optional package libxslt|libxslt1 is unavailable" >&2

add_package ADDITIONAL_LIST docbook-style-xsl || add_package ADDITIONAL_LIST docbook-xsl-stylesheets ||
echo "Optional package docbook-style-xsl|docbook-xsl-stylesheets is unavailable" >&2

add_package ADDITIONAL_LIST brotli-devel || add_packages ADDITIONAL_LIST libbrotli-devel libbrotlidec1 ||
echo "Optional packages brotli-devel|libbrotli-devel is unavailable" >&2

add_package ADDITIONAL_LIST libnl3-devel || add_package ADDITIONAL_LIST libnl-devel ||
echo "Optional package libnl3-devel|libnl-devel are unavailable" >&2

add_package ADDITIONAL_LIST ilbc-devel ||
echo "Optional package ilbc-devel is unavailable" >&2

# opus-devel: RHEL/CentOS, Fedora
# libopus-devel: OpenSUSE
add_package ADDITIONAL_LIST opus-devel || add_package ADDITIONAL_LIST libopus-devel ||
echo "Optional package opus-devel|libopus-devel is unavailable" >&2

add_package ADDITIONAL_LIST bcg729-devel ||
echo "Optional package bcg729-devel is unavailable" >&2

add_package ADDITIONAL_LIST zlib-ng-devel ||
echo "Optional package zlib-ng-devel is unavailable" >&2

# RHEL 8 / CentOS 8 are missing the -devel packages for sbc and libsmi due to
# RH deciding not to ship all -devel packages.
# https://wiki.centos.org/FAQ/CentOS8/UnshippedPackages
# There are CentOS bugs filed to add them to the Devel repository and eventually
# RHEL 8 CRB / CentOS PowerTools, but make them optional for now.
# https://bugs.centos.org/view.php?id=16504
# https://bugs.centos.org/view.php?id=17824
add_package ADDITIONAL_LIST sbc-devel ||
echo "Optional package sbc-devel is unavailable"

add_package ADDITIONAL_LIST libsmi-devel ||
echo "Optional package libsmi-devel is unavailable"

add_package ADDITIONAL_LIST opencore-amr-devel ||
echo "Optional package opencore-amr-devel is unavailable" >&2

ACTUAL_LIST=$BASIC_LIST

# Now arrange for optional support libraries
if [ $ADDITIONAL -ne 0 ]
then
	ACTUAL_LIST="$ACTUAL_LIST $ADDITIONAL_LIST"
fi

if [ $RPMDEPS -ne 0 ]
then
	ACTUAL_LIST="$ACTUAL_LIST $RPMDEPS_LIST"
fi

# shellcheck disable=SC2086
$PM $PM_OPT install $ACTUAL_LIST $OPTIONS

if [ $ADDITIONAL -eq 0 ]
then
	echo -e "\n*** Optional packages not installed. Rerun with --install-optional to have them.\n"
fi

if [ $RPMDEPS -eq 0 ]
then
	printf "\n*** RPM packages build deps not installed. Rerun with --install-rpm-deps to have them.\n"
fi
