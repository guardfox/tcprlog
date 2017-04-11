#!/usr/bin/perl

use strict;
use warnings;

our $root_mib = qw(.1.3.6.1.4.99.1);
our $snmp_data_dir = "/var/snmp_data";
our $stale_timeout = "120";


-e "$0.locals" && require "$0.locals";

$root_mib =~ s/\.$//;
$root_mib =~ s/^\.//;
$| = 1;
my ($reply,$output);
select X;
$| = 1;
select STDOUT;
while(<>) {
   ($reply,$output) = ("","");
   my ($mib);
   chomp;
   if($_ eq "PING") {
      print "PONG\n";
      next;
   }
   $mib = <>;
   if(!defined $mib or !$mib or $mib !~ /^\Q.$root_mib.\E(.*)$/) {
      print $mib;
      print "string\n";
      chomp $mib;
      print "$mib do not match $root_mib...\n";
      next;
   }
   &status_from_file($1);
   print $mib;
   print "string\n";
   if($reply =~ /^\s*\n$/) {$reply = "ERR NO DATA\n"};
   if($reply !~ /\n$/) { $reply = "ERR no EOL in reply\n";}
   print $reply;
}
exit;
#######################################



sub status_from_file
{
   my ($status_file) = shift;
   $status_file = "$snmp_data_dir/$status_file";
   if(my @sss = stat $status_file) {
      if($sss[9] + $stale_timeout > time) {
         if(open (ST,$status_file)) {
            $reply = join "",<ST>;
            close ST;
         } else {
            $reply = "ERR open $status_file: $!\n";
         }
      } else {
         $reply = "ERR $status_file is stale\n";
      }
   } else {
      $reply = "ERR stat $status_file: $!\n";
   }
}

