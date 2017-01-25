/*
 * Copyright 2017 Colin Walters <walters@verbum.org>
 * Based on original bupsplit.c:
 * Copyright 2011 Avery Pennarun. All rights reserved.
 *
 * (This license applies to bupsplit.c and bupsplit.h only.)
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AVERY PENNARUN ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

extern crate libc;

use std::slice;

// According to librsync/rollsum.h:
// "We should make this something other than zero to improve the
// checksum algorithm: tridge suggests a prime number."
// apenwarr: I unscientifically tried 0 and 7919, and they both ended up
// slightly worse than the librsync value of 31 for my arbitrary test data.
const ROLLSUM_CHAR_OFFSET: u32 = 31;

// Previously in the header file
const BUP_BLOBBITS: u32= 13;
const BUP_BLOBSIZE: u32 = (1<<BUP_BLOBBITS);
const BUP_WINDOWBITS: u32 = 7;
const BUP_WINDOWSIZE: u32 = (1<<(BUP_WINDOWBITS-1));

struct Rollsum {
    s1: u32,
    s2: u32,
    window: [u8; BUP_WINDOWSIZE as usize],
    wofs: i32,
}

impl Rollsum {
    pub fn new() -> Rollsum {
        Rollsum { s1: BUP_WINDOWSIZE * ROLLSUM_CHAR_OFFSET,
                  s2: BUP_WINDOWSIZE * (BUP_WINDOWSIZE-1) * ROLLSUM_CHAR_OFFSET,
                  window: [0; 64],
                  wofs: 0
        }
    }

    // These formulas are based on rollsum.h in the librsync project.
    pub fn add(&mut self, drop: u8, add: u8) -> () {
        let drop_expanded = drop as u32;
        let add_expanded = add as u32;
        self.s1 = self.s1.wrapping_add(add_expanded.wrapping_sub(drop_expanded));
        self.s2 = self.s2.wrapping_add(self.s1.wrapping_sub(BUP_WINDOWSIZE * (drop_expanded + ROLLSUM_CHAR_OFFSET)));
    }

    pub fn roll(&mut self, ch: u8) -> () {
        let wofs = self.wofs as usize;
        let dval = self.window[wofs];
        self.add(dval, ch);
        self.window[wofs] = ch;
        self.wofs = (self.wofs + 1) % (BUP_WINDOWSIZE as i32);
    }

    pub fn digest(&self) -> u32 {
        (self.s1 << 16) | (self.s2 & 0xFFFF)
    }
}

#[no_mangle]
pub extern fn bupsplit_sum(buf: *const u8, ofs: libc::size_t, len: libc::size_t) -> u32 {
    let sbuf = unsafe {
        assert!(!buf.is_null());
        slice::from_raw_parts(buf.offset(ofs as isize), (len - ofs) as usize)
    };

    let mut r = Rollsum::new();
    for x in sbuf {
        r.roll(*x);
    }
    r.digest()
}

#[no_mangle]
pub extern fn bupsplit_find_ofs(buf: *const u8, len: libc::size_t,
                                bits: *mut libc::c_int) -> libc::c_int
{
    let sbuf = unsafe {
        assert!(!buf.is_null());
        slice::from_raw_parts(buf, len as usize)
    };

    let mut r = Rollsum::new();
    for x in sbuf {
        r.roll(*x);
	      if (r.s2 & (BUP_BLOBSIZE-1)) == ((u32::max_value()) & (BUP_BLOBSIZE-1)) {
	          if !bits.is_null() {
                let mut sum = r.digest() >> BUP_BLOBBITS;
                let mut rbits : libc::c_int = BUP_BLOBBITS as i32;
                while sum & 1 != 0 {
                    sum = sum >> 1;
                    rbits = rbits + 1;
                }
                unsafe {
                    *bits = rbits;
                }
            }
            return len as i32
        }
    }
    0
}
