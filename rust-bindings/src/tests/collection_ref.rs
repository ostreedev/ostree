#![cfg(feature = "v2018_6")]

use crate::CollectionRef;
use std::collections::hash_map::DefaultHasher;
use std::hash::{Hash, Hasher};

fn hash(v: &impl Hash) -> u64 {
    let mut s = DefaultHasher::new();
    v.hash(&mut s);
    s.finish()
}

#[test]
fn same_value_should_be_equal() {
    let r = CollectionRef::new(Some("io.gitlab.fkrull"), "ref");
    assert_eq!(r, r);
}

#[test]
fn equal_values_should_be_equal() {
    let a = CollectionRef::new(Some("io.gitlab.fkrull"), "ref");
    let b = CollectionRef::new(Some("io.gitlab.fkrull"), "ref");
    assert_eq!(a, b);
}

#[test]
fn equal_values_without_collection_id_should_be_equal() {
    let a = CollectionRef::new(None, "ref-name");
    let b = CollectionRef::new(None, "ref-name");
    assert_eq!(a, b);
}

#[test]
fn different_values_should_not_be_equal() {
    let a = CollectionRef::new(Some("io.gitlab.fkrull"), "ref1");
    let b = CollectionRef::new(Some("io.gitlab.fkrull"), "ref2");
    assert_ne!(a, b);
}

#[test]
fn hash_for_equal_values_should_be_equal() {
    let a = CollectionRef::new(Some("io.gitlab.fkrull"), "ref");
    let b = CollectionRef::new(Some("io.gitlab.fkrull"), "ref");
    assert_eq!(hash(&a), hash(&b));
}

#[test]
fn hash_for_values_with_different_collection_id_should_be_different() {
    let a = CollectionRef::new(Some("io.gitlab.fkrull1"), "ref");
    let b = CollectionRef::new(Some("io.gitlab.fkrull2"), "ref");
    assert_ne!(hash(&a), hash(&b));
}

#[test]
fn hash_for_values_with_different_ref_id_should_be_different() {
    let a = CollectionRef::new(Some("io.gitlab.fkrull"), "ref-1");
    let b = CollectionRef::new(Some("io.gitlab.fkrull"), "ref-2");
    assert_ne!(hash(&a), hash(&b));
}

#[test]
fn hash_should_be_different_if_collection_id_is_absent() {
    let a = CollectionRef::new(Some("io.gitlab.fkrull"), "ref");
    let b = CollectionRef::new(None, "ref");
    assert_ne!(hash(&a), hash(&b));
}

#[test]
fn clone_should_be_equal_to_original_value() {
    let a = CollectionRef::new(Some("io.gitlab.fkrull"), "ref");
    let b = a.clone();
    assert_eq!(a, b);
}
