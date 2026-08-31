#!/usr/bin/env perl

use strict;
use warnings;
use bytes;
use Encode qw(decode FB_CROAK);

sub contains_protected_metadata
{
    my ($text, $policy) = @_;
    my $slash = chr(47);

    my @protected_roots = (
        $slash . 'Users' . $slash,
        $slash . 'home' . $slash,
        $slash . 'root' . $slash,
    );
    if ($policy eq 'artifact') {
        push @protected_roots,
            $slash . 'private' . $slash . 'var' . $slash . 'folders' . $slash,
            $slash . 'opt' . $slash . 'homebrew' . $slash . 'Cellar' . $slash,
            $slash . 'opt' . $slash . 'homebrew' . $slash . 'opt' . $slash,
            $slash . 'usr' . $slash . 'local' . $slash . 'Cellar' . $slash,
            $slash . 'usr' . $slash . 'local' . $slash . 'opt' . $slash;
    }

    for my $protected_root (@protected_roots) {
        return 1 if index($text, $protected_root) >= 0;
    }

    return 1 if $text =~ /[A-Za-z]:[\\\/]+Users[\\\/]+/i;

    while ($text =~ m{
        (?<![0-9])
        ([0-9]{1,3})[.]([0-9]{1,3})[.]([0-9]{1,3})[.]([0-9]{1,3})
        (?![0-9])
    }gx) {
        my ($first, $second, $third, $fourth) =
            ($1 + 0, $2 + 0, $3 + 0, $4 + 0);

        next if $first > 255 || $second > 255 ||
            $third > 255 || $fourth > 255;

        return 1 if $first == 10;
        return 1 if $first == 169 && $second == 254;
        return 1 if $first == 192 && $second == 168;
        return 1 if $first == 172 && $second >= 16 && $second <= 31;
    }

    return 0;
}

sub pathname_is_unsafe
{
    my ($pathname) = @_;
    my $decoded_pathname;

    return 1 if !length($pathname);
    return 1 if $pathname =~ m{(?:^|/)::};

    my $decode_succeeded = eval {
        $decoded_pathname = decode('UTF-8', $pathname, FB_CROAK);
        1;
    };
    return 1 if !$decode_succeeded;

    {
        no bytes;
        return 1 if $decoded_pathname =~ /[\p{Cc}\p{Cf}\p{Cs}\p{Zl}\p{Zp}]/;
    }

    return 0;
}

sub pathname_contains_protected_metadata
{
    my ($pathname, $policy) = @_;

    return 1 if pathname_is_unsafe($pathname);
    return contains_protected_metadata($pathname, $policy);
}

sub pathname_file_contains_protected_metadata
{
    my ($file_path, $policy) = @_;
    my $pathname = '';

    open my $file_handle, '<:raw', $file_path or return 2;
    while (1) {
        my $read_count = sysread($file_handle, my $chunk, 1024 * 1024);
        if (!defined($read_count)) {
            close $file_handle;
            return 2;
        }
        last if $read_count == 0;
        $pathname .= $chunk;
    }
    close $file_handle or return 2;

    return pathname_contains_protected_metadata($pathname, $policy);
}

sub file_contains_protected_metadata
{
    my ($file_path, $policy) = @_;
    my $contents = '';

    open my $file_handle, '<:raw', $file_path or return 2;
    while (1) {
        my $read_count = sysread($file_handle, my $chunk, 1024 * 1024);
        if (!defined($read_count)) {
            close $file_handle;
            return 2;
        }
        last if $read_count == 0;
        $contents .= $chunk;
    }
    close $file_handle or return 2;

    return 1 if contains_protected_metadata($contents, $policy);

    my @encoded_ascii_runs = (
        qr/(?:[\x20-\x7e]\x00){4,}/,
        qr/(?:\x00[\x20-\x7e]){4,}/,
        qr/(?:[\x20-\x7e]\x00\x00\x00){4,}/,
        qr/(?:\x00\x00\x00[\x20-\x7e]){4,}/,
    );

    for my $encoded_ascii_run (@encoded_ascii_runs) {
        while ($contents =~ /($encoded_ascii_run)/g) {
            my $candidate = $1;
            $candidate =~ tr/\x00//d;
            return 1 if contains_protected_metadata($candidate, $policy);
        }
    }

    return 0;
}

exit 2 if @ARGV != 2;
my $mode_argument = shift @ARGV;
my $input = shift @ARGV;

if ($mode_argument eq '--source' || $mode_argument eq '--artifact') {
    my $policy = substr($mode_argument, 2);
    exit file_contains_protected_metadata($input, $policy);
}
if ($mode_argument eq '--source-path' ||
        $mode_argument eq '--artifact-path') {
    my $policy = $mode_argument =~ /^--source/ ? 'source' : 'artifact';
    exit pathname_contains_protected_metadata($input, $policy);
}
if ($mode_argument eq '--source-path-file' ||
        $mode_argument eq '--artifact-path-file') {
    my $policy = $mode_argument =~ /^--source/ ? 'source' : 'artifact';
    exit pathname_file_contains_protected_metadata($input, $policy);
}

exit 2;
