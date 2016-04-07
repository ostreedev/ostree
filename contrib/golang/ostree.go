// +build linux

// Public API specification for libostree Go bindings

package ostree

import (
       "unsafe"
)

// #cgo pkg-config: ostree-1
// #include <stdlib.h>
// #include <glib.h>
// #include <ostree.h>
// #include "ostree.go.h"
import "C"

type Repo struct {
	*GObject
}

func RepoGetType() GType {
	return GType(C.ostree_repo_get_type())
}

func (r *Repo) native() *C.OstreeRepo {
	return (*C.OstreeRepo)(r.ptr)
}

func repoFromNative(p *C.OstreeRepo) *Repo {
	if p == nil {
		return nil
	}
	o := GObjectNew(unsafe.Pointer(p))
	r := &Repo{o}
	return r
}

func RepoNewOpen(path string) (*Repo, error) {
	var cerr *C.GError = nil
	cpath := C.CString(path)
	pathc := C.g_file_new_for_path(cpath);
	defer C.g_object_unref(C.gpointer(pathc))
	crepo := C.ostree_repo_new(pathc)
	repo := repoFromNative(crepo);
	r := GoBool(C.ostree_repo_open(repo.native(), nil, &cerr))
	if !r {
		return nil, ConvertGError(cerr)
	}
	return repo, nil
}

func (r *Repo) GetParent() *Repo {
	return repoFromNative(C.ostree_repo_get_parent(r.native()))
}

type ObjectType int

const (
	OBJECT_TYPE_FILE               ObjectType = C.OSTREE_OBJECT_TYPE_FILE
	OBJECT_TYPE_DIR_TREE                      = C.OSTREE_OBJECT_TYPE_DIR_TREE
	OBJECT_TYPE_DIR_META                      = C.OSTREE_OBJECT_TYPE_DIR_META
	OBJECT_TYPE_COMMIT                        = C.OSTREE_OBJECT_TYPE_COMMIT
	OBJECT_TYPE_TOMBSTONE_COMMIT              = C.OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT
)       

func (repo *Repo) LoadVariant(t ObjectType, checksum string) (*GVariant, error) {
	var cerr *C.GError = nil
	var cvariant *C.GVariant = nil

	r := GoBool(C.ostree_repo_load_variant(repo.native(), C.OstreeObjectType(t), C.CString(checksum), &cvariant, &cerr))
	if !r {
		return nil, ConvertGError(cerr)
	}
	variant := GVariantNew(unsafe.Pointer(cvariant))
	return variant, nil
}

func (repo *Repo) ResolveRev(ref string) (string, error) {
	var cerr *C.GError = nil
	var crev *C.char = nil

	r := GoBool(C.ostree_repo_resolve_rev(repo.native(), C.CString(ref), GBool(true), &crev, &cerr))
	if !r {
		return "", ConvertGError(cerr)
	}
	defer C.free(unsafe.Pointer(crev))
	return C.GoString(crev), nil
}

func (commit *GVariant) CommitGetMetadataKeyString(key string) (string, error) {
	cmeta := GVariantNew(unsafe.Pointer(C.g_variant_get_child_value(commit.native(), 0)))
	return cmeta.LookupString(key)
}
