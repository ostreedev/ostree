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

type RepoFile struct {
	ptr unsafe.Pointer
}

type MutableTree struct {
	*GObject
}

func MutableTreeNew() *MutableTree {
	p := C.ostree_mutable_tree_new()
	o := GObjectNew(unsafe.Pointer(p))
	r := &MutableTree{o}
	return r
}

func (r *RepoFile) native() *C.OstreeRepoFile {
	return (*C.OstreeRepoFile)(r.ptr)
}

func (r *MutableTree) native() *C.OstreeMutableTree {
	return (*C.OstreeMutableTree)(r.ptr)
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

func (repo *Repo) PrepareTransaction() (bool, error) {
	var cerr *C.GError = nil
	var resume C.gboolean

	r := GoBool(C.ostree_repo_prepare_transaction(repo.native(), &resume, nil, &cerr))
	if !r {
		return false, ConvertGError(cerr)
	}
	return GoBool(resume), nil
}

type OstreeRepoTransactionStats struct {
	metadata_objects_total int32
	metadata_objects_written int32
	content_objects_total int32
	content_objects_written int32
	content_bytes_written uint64
}

func (repo *Repo) CommitTransaction() (*OstreeRepoTransactionStats, error) {
	var cerr *C.GError = nil
	var stats OstreeRepoTransactionStats = OstreeRepoTransactionStats{}
	statsPtr := (*C.OstreeRepoTransactionStats)(unsafe.Pointer(&stats))
	r := GoBool(C.ostree_repo_commit_transaction(repo.native(), statsPtr, nil, &cerr))
	if !r {
		return nil, ConvertGError(cerr)
	}
	return &stats, nil
}

func (repo *Repo) WriteCommit(parent string, subject string, body string, metadata *GVariant, root *RepoFile) (string, error) {
	var cerr *C.GError = nil
	var out *C.char = nil

	var cParent *C.char = nil
	var cSubject *C.char = nil
	var cBody *C.char = nil

	if parent != "" {
		cParent = C.CString(parent)
	}
	if subject != "" {
		cSubject = C.CString(subject)
	}
	if body != "" {
		cBody = C.CString(body)
	}

	r := GoBool(C.ostree_repo_write_commit(repo.native(), cParent, cSubject, cBody, metadata.native(), root.native(), &out, nil, &cerr))
	if !r {
		return "", ConvertGError(cerr)
	}
	if out != nil {
		defer C.free(unsafe.Pointer(out))
	}
	return C.GoString(out), nil
}

func (repo *Repo) WriteDirectoryToMtree(dir string, mtree *MutableTree) error {
	var cerr *C.GError = nil
	var dirC *C.GFile = C.g_file_new_for_path (C.CString(dir))
	var modifier *C.OstreeRepoCommitModifier = nil
	mtreeC := mtree.native()
	defer C.g_object_unref(C.gpointer(dirC))

	r := GoBool(C.ostree_repo_write_directory_to_mtree(repo.native(), dirC, mtreeC, modifier, nil, &cerr))
	if !r {
		return ConvertGError(cerr)
	}
	return nil
}

func (repo *Repo) WriteMtree(mtree *MutableTree) (*RepoFile, error) {
	var cerr *C.GError = nil
	var outFile *C.GFile = nil
	r := GoBool(C.ostree_repo_write_mtree(repo.native(), mtree.native(), &outFile, nil, &cerr))
	if !r {
		return nil, ConvertGError(cerr)
	}

	repoFile := &RepoFile{unsafe.Pointer(outFile)}
	return repoFile, nil
}

func (repo *Repo) TransactionSetRef(remote string, ref string, checksum string) {
	var cRemote *C.char = nil
	var cRef *C.char = nil
	var cChecksum *C.char = nil

	if remote != "" {
		cRemote = C.CString(remote)
	}
	if ref != "" {
		cRef = C.CString(ref)
	}
	if checksum != "" {
		cChecksum = C.CString(checksum)
	}
	C.ostree_repo_transaction_set_ref(repo.native(), cRemote, cRef, cChecksum)
}
