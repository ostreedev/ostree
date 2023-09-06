use crate::CollectionRef;
use glib::translate::ToGlibPtr;
use std::ffi::CStr;

trait AsNonnullPtr
where
    Self: Sized,
{
    fn as_nonnull_ptr(&self) -> Option<Self>;
}

impl<T> AsNonnullPtr for *mut T {
    fn as_nonnull_ptr(&self) -> Option<Self> {
        if self.is_null() {
            None
        } else {
            Some(*self)
        }
    }
}

impl CollectionRef {
    /// Get the collection ID from this `CollectionRef`.
    ///
    /// # Returns
    /// Since the value may not be valid UTF-8, `&CStr` is returned. You can safely turn it into a
    /// `&str` using the `as_str` method.
    ///
    /// If no collection ID was set in the `CollectionRef`, `None` is returned.
    pub fn collection_id(&self) -> Option<&CStr> {
        let inner = ToGlibPtr::<*const ffi::OstreeCollectionRef>::to_glib_none(self).0;
        unsafe {
            (*inner)
                .collection_id
                .as_nonnull_ptr()
                .map(|ptr| CStr::from_ptr(ptr))
        }
    }

    /// Get the ref name from this `CollectionRef`.
    ///
    /// # Returns
    /// Since the value may not be valid UTF-8, `&CStr` is returned. You can safely turn it into a
    /// `&str` using the `as_str` method.
    pub fn ref_name(&self) -> &CStr {
        let inner = ToGlibPtr::<*const ffi::OstreeCollectionRef>::to_glib_none(self).0;
        unsafe { CStr::from_ptr((*inner).ref_name) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn should_get_collection_id() {
        let collection_ref = CollectionRef::new(Some("collection.id"), "ref");
        let id = collection_ref.collection_id().unwrap().to_str().unwrap();

        assert_eq!(id, "collection.id");
    }

    #[test]
    fn should_get_none_collection_id() {
        let collection_ref = CollectionRef::new(None, "ref");
        let id = collection_ref.collection_id();

        assert_eq!(id, None);
    }

    #[test]
    fn should_get_ref_name() {
        let collection_ref = CollectionRef::new(Some("collection.id"), "ref-name");
        let ref_name = collection_ref.ref_name().to_str().unwrap();

        assert_eq!(ref_name, "ref-name");
    }
}
