# $OpenBSD: PackingList.pm,v 1.17 2004/08/05 23:36:40 espie Exp $
#
# Copyright (c) 2003 Marc Espie.
# 
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer.
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in the
#    documentation and/or other materials provided with the distribution.
# 
# THIS SOFTWARE IS PROVIDED BY THE OPENBSD PROJECT AND CONTRIBUTORS
# ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE OPENBSD
# PROJECT OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

use strict;
use warnings;
package OpenBSD::PackingList;

use OpenBSD::PackingElement;
use OpenBSD::PackageInfo;

sub new
{
	my $class = shift;
	bless {state => 
	    {default_owner=>'root', 
	     default_group=>'bin', 
	     default_mode=> 0444} }, $class;
}

sub read
{
	my ($a, $fh, $code) = @_;
	my $plist;
	if (ref $a) {
		$plist = $a;
	} else {
		$plist = new $a;
	}
	$code = \&defaultCode if !defined $code;
	&$code($fh,
		sub {
			local $_ = shift;
			next if m/^\s*$/;
			chomp;
			OpenBSD::PackingElement::Factory($_, $plist);
		});
	return $plist;
}

sub defaultCode
{
	my ($fh, $cont) = @_;
	local $_;
	while (<$fh>) {
		&$cont($_);
	}
}

sub DirrmOnly
{
	my ($fh, $cont) = @_;
	local $_;
	while (<$fh>) {
		next unless m/^\@(?:cwd|dirrm|dir|fontdir|mandir|name)\b/ || m/^\@(?:sample|extra)\b.*\/$/ || m/^[^\@].*\/$/;
		&$cont($_);
	}
}

sub FilesOnly
{
	my ($fh, $cont) = @_;
	local $_;
	while (<$fh>) {
	    	next unless m/^\@(?:cwd|name|info|man|file)\b/ || !m/^\@/;
		&$cont($_);
	}
}

sub ConflictOnly
{
	my ($fh, $cont) = @_;
	local $_;
	while (<$fh>) {
	    	next unless m/^\@(?:pkgcfl|option|name)\b/;
		&$cont($_);
	}
}

sub SharedStuffOnly
{
	my ($fh, $cont) = @_;
	local $_;
MAINLOOP:
	while (<$fh>) {
		if (m/^\@shared\b/) {
			&$cont($_);
			while(<$fh>) {
				redo MAINLOOP unless m/^\@(?:md5|size|symlink|link)\b/;
				    m/^\@size\b/ || m/^\@symlink\b/ || 
				    m/^\@link\b/;
				&$cont($_);
			}
		} else {
			next unless m/^\@(?:cwd|dirrm|name)\b/;
		}
		&$cont($_);
	}
}

sub write
{
	my ($self, $fh, $w) = @_;

	$w = sub {
		my ($o, $fh) = @_;
		$o->write($fh);
	    } unless defined $w;

	if (defined $self->{cvstags}) {
		for my $item (@{$self->{cvstags}}) {
			$item->write($fh);
		}
	}
	for my $unique_item (qw(name no-default-conflict extrainfo arch)) {
		$self->{$unique_item}->write($fh) if defined $self->{$unique_item};
	}
	for my $listname (qw(pkgcfl pkgdep newdepend libdepend items)) {
		if (defined $self->{$listname}) {
			for my $item (@{$self->{$listname}}) {
				$item->write($fh);
			}
		}
	}
	for my $special (OpenBSD::PackageInfo::info_names()) {
		$self->{$special}->write($fh) if defined $self->{$special};
	}
}

sub fromfile
{
	my ($a, $fname, $code) = @_;
	open(my $fh, '<', $fname) or return undef;
	my $plist = $a->read($fh, $code);
	close($fh);
	return $plist;
}

sub tofile
{
	my ($self, $fname) = @_;
	open(my $fh, '>', $fname) or return undef;
	$self->write($fh);
	close($fh) or return undef;
	return 1;
}

sub add2list
{
	my ($plist, $object) = @_;
	my $category = $object->category();
	$plist->{$category} = [] unless defined $plist->{$category};
	push @{$plist->{$category}}, $object;
}

sub addunique
{
	my ($plist, $object) = @_;
	my $category = $object->category();
	if (defined $plist->{$category}) {
		die "Duplicate $category in plist\n";
	}
	$plist->{$category} = $object;
}

sub has
{
	my ($plist, $name) = @_;
	return defined $plist->{$name};
}

sub get
{
	my ($plist, $name) = @_;
	return $plist->{$name};
}

sub pkgname($)
{
	my $self = shift;
	return $self->{name}->{name};
}

sub pkgbase($)
{
	my $self = shift;

	if (defined $self->{localbase}) {
		return $self->{localbase}->{name};
	} else {
		return '/usr/local';
	}
}

# allows the autoloader to work correctly
sub DESTROY
{
}

1;
