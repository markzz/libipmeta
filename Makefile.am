#
# libipmeta
#
# Alistair King, CAIDA, UC San Diego
# corsaro-info@caida.org
#
# Copyright (C) 2012 The Regents of the University of California.
#
# This file is part of libipmeta.
#
# libipmeta is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# libipmeta is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with libipmeta.  If not, see <http://www.gnu.org/licenses/>.
#

SUBDIRS = common lib tools
AM_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/common \
	-I$(top_srcdir)/lib \
	-I$(top_srcdir)/lib/datastructures \
	-I$(top_srcdir)/lib/providers

EXTRA_DIST = 

ACLOCAL_AMFLAGS = -I m4

CLEANFILES = *~

clang-format:
	find . -type f -name "*.[ch]" -not -path "./common/*" -exec \
		clang-format -style=file -i {} \;

.PHONY: clang-format

