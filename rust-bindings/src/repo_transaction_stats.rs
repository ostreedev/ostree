use glib::translate::FromGlibPtrFull;

glib::wrapper! {
    /// A list of statistics for each transaction that may be interesting for reporting purposes.
    #[derive(Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
    pub struct RepoTransactionStats(Boxed<ffi::OstreeRepoTransactionStats>);

    match fn {
        copy => |ptr| glib::gobject_ffi::g_boxed_copy(ffi::ostree_repo_transaction_stats_get_type(), ptr as *mut _) as *mut ffi::OstreeRepoTransactionStats,
        free => |ptr| glib::gobject_ffi::g_boxed_free(ffi::ostree_repo_transaction_stats_get_type(), ptr as *mut _),
        type_ => || ffi::ostree_repo_transaction_stats_get_type(),
    }
}

impl RepoTransactionStats {
    /// The total number of metadata objects in the repository after this transaction has completed.
    pub fn get_metadata_objects_total(&self) -> usize {
        self.inner.metadata_objects_total as usize
    }

    /// The number of metadata objects that were written to the repository in this transaction.
    pub fn get_metadata_objects_written(&self) -> usize {
        self.inner.metadata_objects_written as usize
    }

    /// The total number of content objects in the repository after this transaction has completed.
    pub fn get_content_objects_total(&self) -> usize {
        self.inner.content_objects_total as usize
    }

    /// The number of content objects that were written to the repository in this transaction.
    pub fn get_content_objects_written(&self) -> usize {
        self.inner.content_objects_written as usize
    }

    /// The amount of data added to the repository, in bytes, counting only content objects.
    pub fn get_content_bytes_written(&self) -> u64 {
        self.inner.content_bytes_written
    }

    /// The amount of cache hits during this transaction.
    pub fn get_devino_cache_hits(&self) -> usize {
        self.inner.devino_cache_hits as usize
    }

    /// Create new uninitialized stats.
    pub(crate) fn uninitialized() -> Self {
        unsafe {
            let stats: ffi::OstreeRepoTransactionStats = std::mem::zeroed();
            Self::from_glib_full(Box::into_raw(Box::new(stats)))
        }
    }
}
