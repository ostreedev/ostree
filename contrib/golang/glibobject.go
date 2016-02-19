/*
 * Copyright (c) 2013 Conformal Systems <info@conformal.com>
 *
 * This file originated from: http://opensource.conformal.com/
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

package ostree

// #cgo pkg-config: glib-2.0 gobject-2.0
// #include <glib.h>
// #include <glib-object.h>
// #include <gio/gio.h>
// #include "glibobject.go.h"
// #include <stdlib.h>
import "C"
import (
	"unsafe"
	"runtime"
	"fmt"
	"errors"
)

func GBool(b bool) C.gboolean {
	if b {
		return C.gboolean(1)
	}
	return C.gboolean(0)
}

func GoBool(b C.gboolean) bool {
	if b != 0 {
		return true
	}
	return false
}

type GError struct {
     ptr unsafe.Pointer
}

func NewGError() GError {
     return GError{nil}
}

func (e *GError) Native() *C.GError {
     if e == nil {
     	return nil
     }
     return (*C.GError)(e.ptr)
}

func ConvertGError(e *C.GError) error {
     defer C.g_error_free(e)
     return errors.New(C.GoString((*C.char)(C._g_error_get_message(e))))
}

type GType uint

func (t GType) Name() string {
     return C.GoString((*C.char)(C.g_type_name(C.GType(t))))
}
     
type GVariant struct {
     ptr unsafe.Pointer
}

func GVariantNew(p unsafe.Pointer) *GVariant {
     o := &GVariant{p}
     runtime.SetFinalizer(o, (*GVariant).Unref)
     return o;
}

func GVariantNewSink(p unsafe.Pointer) *GVariant {
     o := &GVariant{p}
     runtime.SetFinalizer(o, (*GVariant).Unref)
     o.RefSink()
     return o;
}

func (v *GVariant) native() *C.GVariant {
     return (*C.GVariant)(v.ptr);
}

func (v *GVariant) Ref() {
	C.g_variant_ref(v.native())
}

func (v *GVariant) Unref() {
     C.g_variant_unref(v.native())
}

func (v *GVariant) RefSink() {
	C.g_variant_ref_sink(v.native())
}

func (v *GVariant) TypeString() string {
	cs := (*C.char)(C.g_variant_get_type_string(v.native()))
	return C.GoString(cs)
}

func (v *GVariant) GetChildValue(i int) *GVariant {
	cchild := C.g_variant_get_child_value(v.native(), C.gsize(i))
	return GVariantNew(unsafe.Pointer(cchild));
}

func (v *GVariant) LookupString(key string) (string, error) {
     ckey := C.CString(key)
     defer C.free(unsafe.Pointer(ckey))
     // TODO: Find a way to have constant C strings in golang
     cstr := C._g_variant_lookup_string(v.native(), ckey)
     if cstr == nil {
     	return "", fmt.Errorf("No such key: %s", key)
     }
     return C.GoString(cstr), nil
}

/*
 * GObject
 */

// IObject is an interface type implemented by Object and all types which embed
// an Object.  It is meant to be used as a type for function arguments which
// require GObjects or any subclasses thereof.
type IObject interface {
	toGObject() *C.GObject
	ToObject() *GObject
}

// Object is a representation of GLib's GObject.
type GObject struct {
	ptr unsafe.Pointer
}

func GObjectNew(p unsafe.Pointer) *GObject {
     o := &GObject{p}
     runtime.SetFinalizer(o, (*GObject).Unref)
     return o;
}

func (v *GObject) Ptr() unsafe.Pointer {
	return v.ptr
}

func (v *GObject) Native() *C.GObject {
	if v == nil || v.ptr == nil {
		return nil
	}
	return (*C.GObject)(v.ptr)
}

func (v *GObject) toGObject() *C.GObject {
	if v == nil {
		return nil
	}
	return v.Native()
}

func (v *GObject) Ref() {
	C.g_object_ref(C.gpointer(v.ptr))
}

func (v *GObject) Unref() {
     C.g_object_unref(C.gpointer(v.ptr))
}

func (v *GObject) RefSink() {
	C.g_object_ref_sink(C.gpointer(v.ptr))
}

func (v *GObject) IsFloating() bool {
	c := C.g_object_is_floating(C.gpointer(v.ptr))
	return GoBool(c)
}

func (v *GObject) ForceFloating() {
	C.g_object_force_floating((*C.GObject)(v.ptr))
}

// GIO types

type GCancellable struct {
     *GObject
}

func (self *GCancellable) native() *C.GCancellable {
	return (*C.GCancellable)(self.ptr)
}

// At the moment, no cancellable API, just pass nil
