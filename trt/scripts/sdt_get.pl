#!/usr/bin/env perl
#  Copyright (c) 2020 International Business Machines
#  All rights reserved.
#
#  SPDX-License-Identifier: BSD-3-Clause
#
#  Authors: Kornilios Kourtis (kou@zurich.ibm.com, kornilios@gmail.com)
#

die "Usage: $0 [binary_file]\n" unless $ARGV[0];

my $fname=$ARGV[0];


my $readelf = $ENV{"READELF"};
if (!defined($readelf)) {
    $readelf = "readelf";
}
$readelf="$readelf -n $fname |";

open(my $P, $readelf) or die "Fail openning $readelf: $!\n";

my $sdt = 0;
my @data = ();
my %cur = ();

while (<$P>) {
    if (/stapsdt/) {
        if (keys %cur) {
            push(@data, { %cur });
            %cur = ();
        }
    } elsif (/\s+Provider:\s+(\S+)/) {
        $cur{"provider"} = $1;
    } elsif (/\s+Name:\s+(\S+)/) {
        $cur{"name"} = $1;
    }
}

if (keys %cur) {
    push(@data, { %cur } );
}

for $h (@data) {
    print $$h{"provider"} . ":" . $$h{"name"} . "\n";
}
