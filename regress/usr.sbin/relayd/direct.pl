#!/usr/bin/perl
#	$OpenBSD: direct.pl,v 1.2 2013/01/04 14:01:49 bluhm Exp $

# Copyright (c) 2010-2013 Alexander Bluhm <bluhm@openbsd.org>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

use strict;
use warnings;
use Socket;
use Socket6;

use Client;
use Server;
require 'funcs.pl';

sub usage {
	die "usage: direct.pl [test-args.pl]\n";
}

my $test;
our %args;
if (@ARGV and -f $ARGV[-1]) {
	$test = pop;
	do $test
	    or die "Do test file $test failed: ", $@ || $!;
}

@ARGV == 0 or usage();

my $s = Server->new(
    func		=> \&read_char,
    %{$args{server}},
    listendomain	=> AF_INET,
    listenaddr		=> "127.0.0.1",
);
my $c = Client->new(
    func		=> \&write_char,
    %{$args{client}},
    connectdomain	=> AF_INET,
    connectaddr		=> "127.0.0.1",
    connectport		=> $s->{listenport},
);

$s->run;
$c->run->up;
$s->up;

$c->down;
$s->down;

check_logs($c, undef, $s, %args);
