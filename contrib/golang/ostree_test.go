// +build linux

// Public API specification for libostree Go bindings

package ostree

import (
	"testing"
)

func TestTypeName(t *testing.T) {
	name := RepoGetType().Name();
	if name != "OstreeRepo" {
		t.Errorf("%s != OstreeRepo");
	}
}

func TestRepoNew(t *testing.T) {
	r, err := RepoNewOpen("/ostree/repo")
	if err != nil {
		t.Errorf("%s", err);
		return
	}
	parent := r.GetParent()
	if parent != nil {
		t.Errorf("Expected no parent")
		return
	}
}

func TestRepoGetMetadataVersion(t *testing.T) {
	r, err := RepoNewOpen("/ostree/repo")
	if err != nil {
		t.Errorf("%s", err);
		return
	}
	commit,err := r.ResolveRev("rhel-atomic-host/7/x86_64/standard")
	if err != nil {
		t.Errorf("%s", err)
		return
	}
	commitv,err := r.LoadVariant(OBJECT_TYPE_COMMIT, commit)
	if err != nil {
		t.Errorf("%s", err)
		return
	}
	ver, err := commitv.CommitGetMetadataKeyString("version")
	if err != nil {
		t.Errorf("%s", err)
		return
	}
	if ver != "7.1.3" {
		t.Errorf("expected 7.1.3")
	}
}
