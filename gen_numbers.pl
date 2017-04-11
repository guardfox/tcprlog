#!/usr/bin/perl

use strict;
use warnings;
use Socket qw(:all);

my ($F_GETFL,$F_SETFL,$O_NONBLOCK);
my $uname = `uname -a`;
$uname =~ s/\s.*//;
if($uname =~ /linux/i) {
   $uname = "linux";
   ($F_GETFL,$F_SETFL,$O_NONBLOCK) = (3,4,2048);
}elsif($uname =~ /freebsd/i) {
   $uname = "freebsd";
   ($F_GETFL,$F_SETFL,$O_NONBLOCK) = (3,4,4);
}else {
   die "can not handle OS $uname\n";
}


our %files;
our %hosts_allow;
our $delay_check = 5;
our $timeout_write = 60;
our $bind_port = 19;
our $bind_addr = "0";


-e "$0.locals" && require "$0.locals";

socket(my $sfd, AF_INET, SOCK_STREAM, 6) || die "socket: $!";
setsockopt($sfd, SOL_SOCKET, SO_REUSEADDR, pack("l", 1)) || die "setsockopt: $!";
bind($sfd, sockaddr_in($bind_port, inet_aton $bind_addr)) || die "bind: $!";
listen($sfd,SOMAXCONN) || die "listen: $!";
my ($peer_addr);
$SIG{CHLD} = \&REAPER;
while(1) {
   if(&activity_fd(0,fileno $sfd, undef) > 0 && accept (Cfd,$sfd)) {
      my ($port, $iaddr) = sockaddr_in(getpeername(Cfd));
      $peer_addr = inet_ntoa($iaddr);
      if(!$hosts_allow{$peer_addr}) {
         warn scalar (localtime) . " rejected $peer_addr:$port\n";
         close Cfd;
         next;
      }
      my $pid;
      if (!defined($pid = fork)) {
         close Cfd;
         next;
      } elsif ($pid) {
         close Cfd;
         next;
      }
      close $sfd;
      last;
   }
}


exit unless &activity_fd(0,fileno Cfd,$delay_check);
chomp(my $l = <Cfd>);
my ($file,$file_inode,$off) = (split /\s+/,$l);
my ($real_file) = $file;
exit if $file_inode && $file_inode !~ /^[0-9]+$/;
exit if !$file or !$files{$file} or !-e $file;
my $cur_file_inode = (stat $file)[1];
if (!defined $off) {$off = 0}
my $status = "L";
if(!defined $file_inode or !$file_inode) {
   $file_inode = $cur_file_inode;
   $status = "O";
} else {
   if($file_inode != $cur_file_inode) {
      my $found = 0;
      for my $f (glob $files{$file}) {
         my @stat_f = stat $f;
         if($stat_f[1] == $file_inode) {
            if($off != $stat_f[7]) {
               $file_inode = $stat_f[1];
               $real_file = $f;
               $found = 1;
            }
            $status = "O";
            last;
         }
      }
      if(!$found) {
         $file_inode = $cur_file_inode;
         $off = 0;
      }
   } else {
      $status = "O";
   }
}
my $size = (stat $real_file)[7];
($size < $off) and die(scalar (localtime) ." $peer_addr $real_file off $off > size $size");
sysopen my $ffd,$real_file,0 or die(scalar (localtime) ." $peer_addr open $real_file: $!");
sysseek ($ffd,$off,0) or die(scalar (localtime) ." $peer_addr seek $real_file off $off : $!");
syswrite Cfd,"$status $file_inode\n" or die(scalar (localtime) ." $peer_addr write init: $!");
setsockopt(Cfd, SOL_SOCKET, SO_SNDBUF, pack("l", 131071)) || die "setsockopt: $!";
my $cur_pos = sysseek $ffd,0,1;

my $flags = fcntl(Cfd, $F_GETFL, 0);
fcntl(Cfd, $F_SETFL, $flags | $O_NONBLOCK);

my $i = 0;
if($cur_pos == (stat $ffd)[7]) {syswrite Cfd,"\a";}
while(1) {
   $size = (stat $ffd)[7];
   if($cur_pos < $size) {
      $i = 0;
      last if &relay_data;
   } elsif($cur_pos != $size) {
      last;
   }
   last if &activity_fd(0,fileno Cfd,$delay_check + 2 - (int rand 5) );
   if($i++ > 11) {
      last if (stat $file)[1] != $file_inode;
      $i = 0;
      last unless syswrite Cfd,"\a";
   }
}

shutdown Cfd,2;
exit;
###
sub activity_fd
{
   my ($dir,$fd,$delay) = (@_);
   my($fd_s) = ('');
   vec($fd_s,$fd,1) = 1;
   if($dir) {
      select('',$fd_s, $fd_s, $delay);
   } else {
      select($fd_s, '', $fd_s, $delay);
   }
}


sub relay_data
{
   my ($len,$off,$written);
   while(1) {
      $len = sysread $ffd,my $buff,131072;
      return 1 unless defined $len;
      last unless $len;
      $off = 0;
      $buff =~ tr/\a/\xff/;
      do {
         if(!&activity_fd(1,fileno Cfd,$timeout_write)) { return 1};
         $written = syswrite Cfd,$buff,$len,$off;
         return 1 if !$written;
         $off += $written;
      } while($off < $len)
   }
   $cur_pos = sysseek $ffd,0,1;
   return 0;
}


sub REAPER {
   1 while ( wait() > 0);
   $SIG{CHLD} = \&REAPER;
}


