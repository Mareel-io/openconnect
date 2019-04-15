#! /usr/bin/env perl
# Copyright 2005-2018 The OpenSSL Project Authors. All Rights Reserved.
#
# Licensed under the Apache License 2.0 (the "License").  You may not use
# this file except in compliance with the License.  You can obtain a copy
# in the file LICENSE in the source distribution or at
# https://www.openssl.org/source/license.html


$flavour = shift;
$output  = shift;
if ($flavour =~ /\./) { $output = $flavour; undef $flavour; }

$win64=0; $win64=1 if ($flavour =~ /[nm]asm|mingw64/ || $output =~ /\.asm$/);

$0 =~ m/(.*[\/\\])[^\/\\]+$/; $dir=$1;
( $xlate="${dir}x86_64-xlate.pl" and -f $xlate ) or
( $xlate="${dir}perlasm/x86_64-xlate.pl" and -f $xlate) or
die "can't locate x86_64-xlate.pl";

open OUT,"| \"$^X\" \"$xlate\" $flavour \"$output\"";
*STDOUT=*OUT;

($arg1,$arg2,$arg3,$arg4)=$win64?("%rcx","%rdx","%r8", "%r9") :	# Win64 order
				 ("%rdi","%rsi","%rdx","%rcx");	# Unix order

print<<___;
.text

.globl	OPENSSL_ia32_cpuid
.type	OPENSSL_ia32_cpuid,\@function,1
.align	16
OPENSSL_ia32_cpuid:
.cfi_startproc
	mov	%rbx,%r8		# save %rbx
.cfi_register	%rbx,%r8

	xor	%eax,%eax
	mov	%rax,8(%rdi)		# clear extended feature flags
	cpuid
	mov	%eax,%r11d		# max value for standard query level

	xor	%eax,%eax
	cmp	\$0x756e6547,%ebx	# "Genu"
	setne	%al
	mov	%eax,%r9d
	cmp	\$0x49656e69,%edx	# "ineI"
	setne	%al
	or	%eax,%r9d
	cmp	\$0x6c65746e,%ecx	# "ntel"
	setne	%al
	or	%eax,%r9d		# 0 indicates Intel CPU
	jz	.Lintel

	cmp	\$0x68747541,%ebx	# "Auth"
	setne	%al
	mov	%eax,%r10d
	cmp	\$0x69746E65,%edx	# "enti"
	setne	%al
	or	%eax,%r10d
	cmp	\$0x444D4163,%ecx	# "cAMD"
	setne	%al
	or	%eax,%r10d		# 0 indicates AMD CPU
	jnz	.Lintel

	# AMD specific
	mov	\$0x80000000,%eax
	cpuid
	cmp	\$0x80000001,%eax
	jb	.Lintel
	mov	%eax,%r10d
	mov	\$0x80000001,%eax
	cpuid
	or	%ecx,%r9d
	and	\$0x00000801,%r9d	# isolate AMD XOP bit, 1<<11

	cmp	\$0x80000008,%r10d
	jb	.Lintel

	mov	\$0x80000008,%eax
	cpuid
	movzb	%cl,%r10		# number of cores - 1
	inc	%r10			# number of cores

	mov	\$1,%eax
	cpuid
	bt	\$28,%edx		# test hyper-threading bit
	jnc	.Lgeneric
	shr	\$16,%ebx		# number of logical processors
	cmp	%r10b,%bl
	ja	.Lgeneric
	and	\$0xefffffff,%edx	# ~(1<<28)
	jmp	.Lgeneric

.Lintel:
	cmp	\$4,%r11d
	mov	\$-1,%r10d
	jb	.Lnocacheinfo

	mov	\$4,%eax
	mov	\$0,%ecx		# query L1D
	cpuid
	mov	%eax,%r10d
	shr	\$14,%r10d
	and	\$0xfff,%r10d		# number of cores -1 per L1D

.Lnocacheinfo:
	mov	\$1,%eax
	cpuid
	movd	%eax,%xmm0		# put aside processor id
	and	\$0xbfefffff,%edx	# force reserved bits to 0
	cmp	\$0,%r9d
	jne	.Lnotintel
	or	\$0x40000000,%edx	# set reserved bit#30 on Intel CPUs
	and	\$15,%ah
	cmp	\$15,%ah		# examine Family ID
	jne	.LnotP4
	or	\$0x00100000,%edx	# set reserved bit#20 to engage RC4_CHAR
.LnotP4:
	cmp	\$6,%ah
	jne	.Lnotintel
	and	\$0x0fff0ff0,%eax
	cmp	\$0x00050670,%eax	# Knights Landing
	je	.Lknights
	cmp	\$0x00080650,%eax	# Knights Mill (according to sde)
	jne	.Lnotintel
.Lknights:
	and	\$0xfbffffff,%ecx	# clear XSAVE flag to mimic Silvermont

.Lnotintel:
	bt	\$28,%edx		# test hyper-threading bit
	jnc	.Lgeneric
	and	\$0xefffffff,%edx	# ~(1<<28)
	cmp	\$0,%r10d
	je	.Lgeneric

	or	\$0x10000000,%edx	# 1<<28
	shr	\$16,%ebx
	cmp	\$1,%bl			# see if cache is shared
	ja	.Lgeneric
	and	\$0xefffffff,%edx	# ~(1<<28)
.Lgeneric:
	and	\$0x00000800,%r9d	# isolate AMD XOP flag
	and	\$0xfffff7ff,%ecx
	or	%ecx,%r9d		# merge AMD XOP flag

	mov	%edx,%r10d		# %r9d:%r10d is copy of %ecx:%edx

	cmp	\$7,%r11d
	jb	.Lno_extended_info
	mov	\$7,%eax
	xor	%ecx,%ecx
	cpuid
	bt	\$26,%r9d		# check XSAVE bit, cleared on Knights
	jc	.Lnotknights
	and	\$0xfff7ffff,%ebx	# clear ADCX/ADOX flag
.Lnotknights:
	movd	%xmm0,%eax		# restore processor id
	and	\$0x0fff0ff0,%eax
	cmp	\$0x00050650,%eax	# Skylake-X
	jne	.Lnotskylakex
	and	\$0xfffeffff,%ebx	# ~(1<<16)
					# suppress AVX512F flag on Skylake-X
.Lnotskylakex:
	mov	%ebx,8(%rdi)		# save extended feature flags
	mov	%ecx,12(%rdi)
.Lno_extended_info:

	bt	\$27,%r9d		# check OSXSAVE bit
	jnc	.Lclear_avx
	xor	%ecx,%ecx		# XCR0
	.byte	0x0f,0x01,0xd0		# xgetbv
	and	\$0xe6,%eax		# isolate XMM, YMM and ZMM state support
	cmp	\$0xe6,%eax
	je	.Ldone
	andl	\$0x3fdeffff,8(%rdi)	# ~(1<<31|1<<30|1<<21|1<<16)
					# clear AVX512F+BW+VL+FIMA, all of
					# them are EVEX-encoded, which requires
					# ZMM state support even if one uses
					# only XMM and YMM :-(
	and	\$6,%eax		# isolate XMM and YMM state support
	cmp	\$6,%eax
	je	.Ldone
.Lclear_avx:
	mov	\$0xefffe7ff,%eax	# ~(1<<28|1<<12|1<<11)
	and	%eax,%r9d		# clear AVX, FMA and AMD XOP bits
	mov	\$0x3fdeffdf,%eax	# ~(1<<31|1<<30|1<<21|1<<16|1<<5)
	and	%eax,8(%rdi)		# clear AVX2 and AVX512* bits
.Ldone:
	shl	\$32,%r9
	mov	%r10d,%eax
	mov	%r8,%rbx		# restore %rbx
.cfi_restore	%rbx
	or	%r9,%rax
	ret
.cfi_endproc
.size	OPENSSL_ia32_cpuid,.-OPENSSL_ia32_cpuid
___


close STDOUT;	# flush
