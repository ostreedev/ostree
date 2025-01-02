// This file was generated by gir (https://github.com/gtk-rs/gir)
// from gir-files
// DO NOT EDIT

use glib::{translate::*};

#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[derive(Clone, Copy)]
#[non_exhaustive]
#[doc(alias = "OstreeDeploymentUnlockedState")]
pub enum DeploymentUnlockedState {
    #[doc(alias = "OSTREE_DEPLOYMENT_UNLOCKED_NONE")]
    None,
    #[doc(alias = "OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT")]
    Development,
    #[doc(alias = "OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX")]
    Hotfix,
    #[doc(alias = "OSTREE_DEPLOYMENT_UNLOCKED_TRANSIENT")]
    Transient,
#[doc(hidden)]
    __Unknown(i32),
}

#[doc(hidden)]
impl IntoGlib for DeploymentUnlockedState {
    type GlibType = ffi::OstreeDeploymentUnlockedState;

    #[inline]
fn into_glib(self) -> ffi::OstreeDeploymentUnlockedState {
match self {
            Self::None => ffi::OSTREE_DEPLOYMENT_UNLOCKED_NONE,
            Self::Development => ffi::OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT,
            Self::Hotfix => ffi::OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX,
            Self::Transient => ffi::OSTREE_DEPLOYMENT_UNLOCKED_TRANSIENT,
            Self::__Unknown(value) => value,
}
}
}

#[doc(hidden)]
impl FromGlib<ffi::OstreeDeploymentUnlockedState> for DeploymentUnlockedState {
    #[inline]
unsafe fn from_glib(value: ffi::OstreeDeploymentUnlockedState) -> Self {
        
match value {
            ffi::OSTREE_DEPLOYMENT_UNLOCKED_NONE => Self::None,
            ffi::OSTREE_DEPLOYMENT_UNLOCKED_DEVELOPMENT => Self::Development,
            ffi::OSTREE_DEPLOYMENT_UNLOCKED_HOTFIX => Self::Hotfix,
            ffi::OSTREE_DEPLOYMENT_UNLOCKED_TRANSIENT => Self::Transient,
            value => Self::__Unknown(value),
}
}
}

#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[derive(Clone, Copy)]
#[non_exhaustive]
#[doc(alias = "OstreeGpgSignatureAttr")]
pub enum GpgSignatureAttr {
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_VALID")]
    Valid,
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_SIG_EXPIRED")]
    SigExpired,
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_KEY_EXPIRED")]
    KeyExpired,
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_KEY_REVOKED")]
    KeyRevoked,
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING")]
    KeyMissing,
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT")]
    Fingerprint,
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_TIMESTAMP")]
    Timestamp,
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_EXP_TIMESTAMP")]
    ExpTimestamp,
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_PUBKEY_ALGO_NAME")]
    PubkeyAlgoName,
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_HASH_ALGO_NAME")]
    HashAlgoName,
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_USER_NAME")]
    UserName,
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_USER_EMAIL")]
    UserEmail,
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT_PRIMARY")]
    FingerprintPrimary,
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP")]
    KeyExpTimestamp,
    #[doc(alias = "OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP_PRIMARY")]
    KeyExpTimestampPrimary,
#[doc(hidden)]
    __Unknown(i32),
}

#[doc(hidden)]
impl IntoGlib for GpgSignatureAttr {
    type GlibType = ffi::OstreeGpgSignatureAttr;

    fn into_glib(self) -> ffi::OstreeGpgSignatureAttr {
match self {
            Self::Valid => ffi::OSTREE_GPG_SIGNATURE_ATTR_VALID,
            Self::SigExpired => ffi::OSTREE_GPG_SIGNATURE_ATTR_SIG_EXPIRED,
            Self::KeyExpired => ffi::OSTREE_GPG_SIGNATURE_ATTR_KEY_EXPIRED,
            Self::KeyRevoked => ffi::OSTREE_GPG_SIGNATURE_ATTR_KEY_REVOKED,
            Self::KeyMissing => ffi::OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING,
            Self::Fingerprint => ffi::OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT,
            Self::Timestamp => ffi::OSTREE_GPG_SIGNATURE_ATTR_TIMESTAMP,
            Self::ExpTimestamp => ffi::OSTREE_GPG_SIGNATURE_ATTR_EXP_TIMESTAMP,
            Self::PubkeyAlgoName => ffi::OSTREE_GPG_SIGNATURE_ATTR_PUBKEY_ALGO_NAME,
            Self::HashAlgoName => ffi::OSTREE_GPG_SIGNATURE_ATTR_HASH_ALGO_NAME,
            Self::UserName => ffi::OSTREE_GPG_SIGNATURE_ATTR_USER_NAME,
            Self::UserEmail => ffi::OSTREE_GPG_SIGNATURE_ATTR_USER_EMAIL,
            Self::FingerprintPrimary => ffi::OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT_PRIMARY,
            Self::KeyExpTimestamp => ffi::OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP,
            Self::KeyExpTimestampPrimary => ffi::OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP_PRIMARY,
            Self::__Unknown(value) => value,
}
}
}

#[doc(hidden)]
impl FromGlib<ffi::OstreeGpgSignatureAttr> for GpgSignatureAttr {
    unsafe fn from_glib(value: ffi::OstreeGpgSignatureAttr) -> Self {
        
match value {
            ffi::OSTREE_GPG_SIGNATURE_ATTR_VALID => Self::Valid,
            ffi::OSTREE_GPG_SIGNATURE_ATTR_SIG_EXPIRED => Self::SigExpired,
            ffi::OSTREE_GPG_SIGNATURE_ATTR_KEY_EXPIRED => Self::KeyExpired,
            ffi::OSTREE_GPG_SIGNATURE_ATTR_KEY_REVOKED => Self::KeyRevoked,
            ffi::OSTREE_GPG_SIGNATURE_ATTR_KEY_MISSING => Self::KeyMissing,
            ffi::OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT => Self::Fingerprint,
            ffi::OSTREE_GPG_SIGNATURE_ATTR_TIMESTAMP => Self::Timestamp,
            ffi::OSTREE_GPG_SIGNATURE_ATTR_EXP_TIMESTAMP => Self::ExpTimestamp,
            ffi::OSTREE_GPG_SIGNATURE_ATTR_PUBKEY_ALGO_NAME => Self::PubkeyAlgoName,
            ffi::OSTREE_GPG_SIGNATURE_ATTR_HASH_ALGO_NAME => Self::HashAlgoName,
            ffi::OSTREE_GPG_SIGNATURE_ATTR_USER_NAME => Self::UserName,
            ffi::OSTREE_GPG_SIGNATURE_ATTR_USER_EMAIL => Self::UserEmail,
            ffi::OSTREE_GPG_SIGNATURE_ATTR_FINGERPRINT_PRIMARY => Self::FingerprintPrimary,
            ffi::OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP => Self::KeyExpTimestamp,
            ffi::OSTREE_GPG_SIGNATURE_ATTR_KEY_EXP_TIMESTAMP_PRIMARY => Self::KeyExpTimestampPrimary,
            value => Self::__Unknown(value),
}
}
}

#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[derive(Clone, Copy)]
#[non_exhaustive]
#[doc(alias = "OstreeObjectType")]
pub enum ObjectType {
    #[doc(alias = "OSTREE_OBJECT_TYPE_FILE")]
    File,
    #[doc(alias = "OSTREE_OBJECT_TYPE_DIR_TREE")]
    DirTree,
    #[doc(alias = "OSTREE_OBJECT_TYPE_DIR_META")]
    DirMeta,
    #[doc(alias = "OSTREE_OBJECT_TYPE_COMMIT")]
    Commit,
    #[doc(alias = "OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT")]
    TombstoneCommit,
    #[doc(alias = "OSTREE_OBJECT_TYPE_COMMIT_META")]
    CommitMeta,
    #[doc(alias = "OSTREE_OBJECT_TYPE_PAYLOAD_LINK")]
    PayloadLink,
    #[doc(alias = "OSTREE_OBJECT_TYPE_FILE_XATTRS")]
    FileXattrs,
    #[doc(alias = "OSTREE_OBJECT_TYPE_FILE_XATTRS_LINK")]
    FileXattrsLink,
#[doc(hidden)]
    __Unknown(i32),
}

#[doc(hidden)]
impl IntoGlib for ObjectType {
    type GlibType = ffi::OstreeObjectType;

    #[inline]
fn into_glib(self) -> ffi::OstreeObjectType {
match self {
            Self::File => ffi::OSTREE_OBJECT_TYPE_FILE,
            Self::DirTree => ffi::OSTREE_OBJECT_TYPE_DIR_TREE,
            Self::DirMeta => ffi::OSTREE_OBJECT_TYPE_DIR_META,
            Self::Commit => ffi::OSTREE_OBJECT_TYPE_COMMIT,
            Self::TombstoneCommit => ffi::OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT,
            Self::CommitMeta => ffi::OSTREE_OBJECT_TYPE_COMMIT_META,
            Self::PayloadLink => ffi::OSTREE_OBJECT_TYPE_PAYLOAD_LINK,
            Self::FileXattrs => ffi::OSTREE_OBJECT_TYPE_FILE_XATTRS,
            Self::FileXattrsLink => ffi::OSTREE_OBJECT_TYPE_FILE_XATTRS_LINK,
            Self::__Unknown(value) => value,
}
}
}

#[doc(hidden)]
impl FromGlib<ffi::OstreeObjectType> for ObjectType {
    #[inline]
unsafe fn from_glib(value: ffi::OstreeObjectType) -> Self {
        
match value {
            ffi::OSTREE_OBJECT_TYPE_FILE => Self::File,
            ffi::OSTREE_OBJECT_TYPE_DIR_TREE => Self::DirTree,
            ffi::OSTREE_OBJECT_TYPE_DIR_META => Self::DirMeta,
            ffi::OSTREE_OBJECT_TYPE_COMMIT => Self::Commit,
            ffi::OSTREE_OBJECT_TYPE_TOMBSTONE_COMMIT => Self::TombstoneCommit,
            ffi::OSTREE_OBJECT_TYPE_COMMIT_META => Self::CommitMeta,
            ffi::OSTREE_OBJECT_TYPE_PAYLOAD_LINK => Self::PayloadLink,
            ffi::OSTREE_OBJECT_TYPE_FILE_XATTRS => Self::FileXattrs,
            ffi::OSTREE_OBJECT_TYPE_FILE_XATTRS_LINK => Self::FileXattrsLink,
            value => Self::__Unknown(value),
}
}
}

#[cfg(feature = "v2018_2")]
#[cfg_attr(docsrs, doc(cfg(feature = "v2018_2")))]
#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[derive(Clone, Copy)]
#[non_exhaustive]
#[doc(alias = "OstreeRepoCheckoutFilterResult")]
pub enum RepoCheckoutFilterResult {
    #[doc(alias = "OSTREE_REPO_CHECKOUT_FILTER_ALLOW")]
    Allow,
    #[doc(alias = "OSTREE_REPO_CHECKOUT_FILTER_SKIP")]
    Skip,
#[doc(hidden)]
    __Unknown(i32),
}

#[cfg(feature = "v2018_2")]
#[cfg_attr(docsrs, doc(cfg(feature = "v2018_2")))]
#[doc(hidden)]
impl IntoGlib for RepoCheckoutFilterResult {
    type GlibType = ffi::OstreeRepoCheckoutFilterResult;

    #[inline]
fn into_glib(self) -> ffi::OstreeRepoCheckoutFilterResult {
match self {
            Self::Allow => ffi::OSTREE_REPO_CHECKOUT_FILTER_ALLOW,
            Self::Skip => ffi::OSTREE_REPO_CHECKOUT_FILTER_SKIP,
            Self::__Unknown(value) => value,
}
}
}

#[cfg(feature = "v2018_2")]
#[cfg_attr(docsrs, doc(cfg(feature = "v2018_2")))]
#[doc(hidden)]
impl FromGlib<ffi::OstreeRepoCheckoutFilterResult> for RepoCheckoutFilterResult {
    #[inline]
unsafe fn from_glib(value: ffi::OstreeRepoCheckoutFilterResult) -> Self {
        
match value {
            ffi::OSTREE_REPO_CHECKOUT_FILTER_ALLOW => Self::Allow,
            ffi::OSTREE_REPO_CHECKOUT_FILTER_SKIP => Self::Skip,
            value => Self::__Unknown(value),
}
}
}

#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[derive(Clone, Copy)]
#[non_exhaustive]
#[doc(alias = "OstreeRepoCheckoutMode")]
pub enum RepoCheckoutMode {
    #[doc(alias = "OSTREE_REPO_CHECKOUT_MODE_NONE")]
    None,
    #[doc(alias = "OSTREE_REPO_CHECKOUT_MODE_USER")]
    User,
#[doc(hidden)]
    __Unknown(i32),
}

#[doc(hidden)]
impl IntoGlib for RepoCheckoutMode {
    type GlibType = ffi::OstreeRepoCheckoutMode;

    #[inline]
fn into_glib(self) -> ffi::OstreeRepoCheckoutMode {
match self {
            Self::None => ffi::OSTREE_REPO_CHECKOUT_MODE_NONE,
            Self::User => ffi::OSTREE_REPO_CHECKOUT_MODE_USER,
            Self::__Unknown(value) => value,
}
}
}

#[doc(hidden)]
impl FromGlib<ffi::OstreeRepoCheckoutMode> for RepoCheckoutMode {
    #[inline]
unsafe fn from_glib(value: ffi::OstreeRepoCheckoutMode) -> Self {
        
match value {
            ffi::OSTREE_REPO_CHECKOUT_MODE_NONE => Self::None,
            ffi::OSTREE_REPO_CHECKOUT_MODE_USER => Self::User,
            value => Self::__Unknown(value),
}
}
}

#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[derive(Clone, Copy)]
#[non_exhaustive]
#[doc(alias = "OstreeRepoCheckoutOverwriteMode")]
pub enum RepoCheckoutOverwriteMode {
    #[doc(alias = "OSTREE_REPO_CHECKOUT_OVERWRITE_NONE")]
    None,
    #[doc(alias = "OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES")]
    UnionFiles,
    #[doc(alias = "OSTREE_REPO_CHECKOUT_OVERWRITE_ADD_FILES")]
    AddFiles,
    #[doc(alias = "OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL")]
    UnionIdentical,
#[doc(hidden)]
    __Unknown(i32),
}

#[doc(hidden)]
impl IntoGlib for RepoCheckoutOverwriteMode {
    type GlibType = ffi::OstreeRepoCheckoutOverwriteMode;

    #[inline]
fn into_glib(self) -> ffi::OstreeRepoCheckoutOverwriteMode {
match self {
            Self::None => ffi::OSTREE_REPO_CHECKOUT_OVERWRITE_NONE,
            Self::UnionFiles => ffi::OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES,
            Self::AddFiles => ffi::OSTREE_REPO_CHECKOUT_OVERWRITE_ADD_FILES,
            Self::UnionIdentical => ffi::OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL,
            Self::__Unknown(value) => value,
}
}
}

#[doc(hidden)]
impl FromGlib<ffi::OstreeRepoCheckoutOverwriteMode> for RepoCheckoutOverwriteMode {
    #[inline]
unsafe fn from_glib(value: ffi::OstreeRepoCheckoutOverwriteMode) -> Self {
        
match value {
            ffi::OSTREE_REPO_CHECKOUT_OVERWRITE_NONE => Self::None,
            ffi::OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES => Self::UnionFiles,
            ffi::OSTREE_REPO_CHECKOUT_OVERWRITE_ADD_FILES => Self::AddFiles,
            ffi::OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_IDENTICAL => Self::UnionIdentical,
            value => Self::__Unknown(value),
}
}
}

#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[derive(Clone, Copy)]
#[non_exhaustive]
#[doc(alias = "OstreeRepoCommitFilterResult")]
pub enum RepoCommitFilterResult {
    #[doc(alias = "OSTREE_REPO_COMMIT_FILTER_ALLOW")]
    Allow,
    #[doc(alias = "OSTREE_REPO_COMMIT_FILTER_SKIP")]
    Skip,
#[doc(hidden)]
    __Unknown(i32),
}

#[doc(hidden)]
impl IntoGlib for RepoCommitFilterResult {
    type GlibType = ffi::OstreeRepoCommitFilterResult;

    #[inline]
fn into_glib(self) -> ffi::OstreeRepoCommitFilterResult {
match self {
            Self::Allow => ffi::OSTREE_REPO_COMMIT_FILTER_ALLOW,
            Self::Skip => ffi::OSTREE_REPO_COMMIT_FILTER_SKIP,
            Self::__Unknown(value) => value,
}
}
}

#[doc(hidden)]
impl FromGlib<ffi::OstreeRepoCommitFilterResult> for RepoCommitFilterResult {
    #[inline]
unsafe fn from_glib(value: ffi::OstreeRepoCommitFilterResult) -> Self {
        
match value {
            ffi::OSTREE_REPO_COMMIT_FILTER_ALLOW => Self::Allow,
            ffi::OSTREE_REPO_COMMIT_FILTER_SKIP => Self::Skip,
            value => Self::__Unknown(value),
}
}
}

#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[derive(Clone, Copy)]
#[non_exhaustive]
#[doc(alias = "OstreeRepoCommitIterResult")]
pub enum RepoCommitIterResult {
    #[doc(alias = "OSTREE_REPO_COMMIT_ITER_RESULT_ERROR")]
    Error,
    #[doc(alias = "OSTREE_REPO_COMMIT_ITER_RESULT_END")]
    End,
    #[doc(alias = "OSTREE_REPO_COMMIT_ITER_RESULT_FILE")]
    File,
    #[doc(alias = "OSTREE_REPO_COMMIT_ITER_RESULT_DIR")]
    Dir,
#[doc(hidden)]
    __Unknown(i32),
}

#[doc(hidden)]
impl IntoGlib for RepoCommitIterResult {
    type GlibType = ffi::OstreeRepoCommitIterResult;

    #[inline]
fn into_glib(self) -> ffi::OstreeRepoCommitIterResult {
match self {
            Self::Error => ffi::OSTREE_REPO_COMMIT_ITER_RESULT_ERROR,
            Self::End => ffi::OSTREE_REPO_COMMIT_ITER_RESULT_END,
            Self::File => ffi::OSTREE_REPO_COMMIT_ITER_RESULT_FILE,
            Self::Dir => ffi::OSTREE_REPO_COMMIT_ITER_RESULT_DIR,
            Self::__Unknown(value) => value,
}
}
}

#[doc(hidden)]
impl FromGlib<ffi::OstreeRepoCommitIterResult> for RepoCommitIterResult {
    #[inline]
unsafe fn from_glib(value: ffi::OstreeRepoCommitIterResult) -> Self {
        
match value {
            ffi::OSTREE_REPO_COMMIT_ITER_RESULT_ERROR => Self::Error,
            ffi::OSTREE_REPO_COMMIT_ITER_RESULT_END => Self::End,
            ffi::OSTREE_REPO_COMMIT_ITER_RESULT_FILE => Self::File,
            ffi::OSTREE_REPO_COMMIT_ITER_RESULT_DIR => Self::Dir,
            value => Self::__Unknown(value),
}
}
}

#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[derive(Clone, Copy)]
#[non_exhaustive]
#[doc(alias = "OstreeRepoMode")]
pub enum RepoMode {
    #[doc(alias = "OSTREE_REPO_MODE_BARE")]
    Bare,
    #[doc(alias = "OSTREE_REPO_MODE_ARCHIVE")]
    Archive,
    #[doc(alias = "OSTREE_REPO_MODE_BARE_USER")]
    BareUser,
    #[doc(alias = "OSTREE_REPO_MODE_BARE_USER_ONLY")]
    BareUserOnly,
    #[doc(alias = "OSTREE_REPO_MODE_BARE_SPLIT_XATTRS")]
    BareSplitXattrs,
#[doc(hidden)]
    __Unknown(i32),
}

#[doc(hidden)]
impl IntoGlib for RepoMode {
    type GlibType = ffi::OstreeRepoMode;

    #[inline]
fn into_glib(self) -> ffi::OstreeRepoMode {
match self {
            Self::Bare => ffi::OSTREE_REPO_MODE_BARE,
            Self::Archive => ffi::OSTREE_REPO_MODE_ARCHIVE,
            Self::BareUser => ffi::OSTREE_REPO_MODE_BARE_USER,
            Self::BareUserOnly => ffi::OSTREE_REPO_MODE_BARE_USER_ONLY,
            Self::BareSplitXattrs => ffi::OSTREE_REPO_MODE_BARE_SPLIT_XATTRS,
            Self::__Unknown(value) => value,
}
}
}

#[doc(hidden)]
impl FromGlib<ffi::OstreeRepoMode> for RepoMode {
    #[inline]
unsafe fn from_glib(value: ffi::OstreeRepoMode) -> Self {
        
match value {
            ffi::OSTREE_REPO_MODE_BARE => Self::Bare,
            ffi::OSTREE_REPO_MODE_ARCHIVE => Self::Archive,
            ffi::OSTREE_REPO_MODE_BARE_USER => Self::BareUser,
            ffi::OSTREE_REPO_MODE_BARE_USER_ONLY => Self::BareUserOnly,
            ffi::OSTREE_REPO_MODE_BARE_SPLIT_XATTRS => Self::BareSplitXattrs,
            value => Self::__Unknown(value),
}
}
}

#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[derive(Clone, Copy)]
#[non_exhaustive]
#[doc(alias = "OstreeRepoRemoteChange")]
pub enum RepoRemoteChange {
    #[doc(alias = "OSTREE_REPO_REMOTE_CHANGE_ADD")]
    Add,
    #[doc(alias = "OSTREE_REPO_REMOTE_CHANGE_ADD_IF_NOT_EXISTS")]
    AddIfNotExists,
    #[doc(alias = "OSTREE_REPO_REMOTE_CHANGE_DELETE")]
    Delete,
    #[doc(alias = "OSTREE_REPO_REMOTE_CHANGE_DELETE_IF_EXISTS")]
    DeleteIfExists,
    #[doc(alias = "OSTREE_REPO_REMOTE_CHANGE_REPLACE")]
    Replace,
#[doc(hidden)]
    __Unknown(i32),
}

#[doc(hidden)]
impl IntoGlib for RepoRemoteChange {
    type GlibType = ffi::OstreeRepoRemoteChange;

    #[inline]
fn into_glib(self) -> ffi::OstreeRepoRemoteChange {
match self {
            Self::Add => ffi::OSTREE_REPO_REMOTE_CHANGE_ADD,
            Self::AddIfNotExists => ffi::OSTREE_REPO_REMOTE_CHANGE_ADD_IF_NOT_EXISTS,
            Self::Delete => ffi::OSTREE_REPO_REMOTE_CHANGE_DELETE,
            Self::DeleteIfExists => ffi::OSTREE_REPO_REMOTE_CHANGE_DELETE_IF_EXISTS,
            Self::Replace => ffi::OSTREE_REPO_REMOTE_CHANGE_REPLACE,
            Self::__Unknown(value) => value,
}
}
}

#[doc(hidden)]
impl FromGlib<ffi::OstreeRepoRemoteChange> for RepoRemoteChange {
    #[inline]
unsafe fn from_glib(value: ffi::OstreeRepoRemoteChange) -> Self {
        
match value {
            ffi::OSTREE_REPO_REMOTE_CHANGE_ADD => Self::Add,
            ffi::OSTREE_REPO_REMOTE_CHANGE_ADD_IF_NOT_EXISTS => Self::AddIfNotExists,
            ffi::OSTREE_REPO_REMOTE_CHANGE_DELETE => Self::Delete,
            ffi::OSTREE_REPO_REMOTE_CHANGE_DELETE_IF_EXISTS => Self::DeleteIfExists,
            ffi::OSTREE_REPO_REMOTE_CHANGE_REPLACE => Self::Replace,
            value => Self::__Unknown(value),
}
}
}

#[derive(Debug, Eq, PartialEq, Ord, PartialOrd, Hash)]
#[derive(Clone, Copy)]
#[non_exhaustive]
#[doc(alias = "OstreeStaticDeltaGenerateOpt")]
pub enum StaticDeltaGenerateOpt {
    #[doc(alias = "OSTREE_STATIC_DELTA_GENERATE_OPT_LOWLATENCY")]
    Lowlatency,
    #[doc(alias = "OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR")]
    Major,
#[doc(hidden)]
    __Unknown(i32),
}

#[doc(hidden)]
impl IntoGlib for StaticDeltaGenerateOpt {
    type GlibType = ffi::OstreeStaticDeltaGenerateOpt;

    #[inline]
fn into_glib(self) -> ffi::OstreeStaticDeltaGenerateOpt {
match self {
            Self::Lowlatency => ffi::OSTREE_STATIC_DELTA_GENERATE_OPT_LOWLATENCY,
            Self::Major => ffi::OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR,
            Self::__Unknown(value) => value,
}
}
}

#[doc(hidden)]
impl FromGlib<ffi::OstreeStaticDeltaGenerateOpt> for StaticDeltaGenerateOpt {
    #[inline]
unsafe fn from_glib(value: ffi::OstreeStaticDeltaGenerateOpt) -> Self {
        
match value {
            ffi::OSTREE_STATIC_DELTA_GENERATE_OPT_LOWLATENCY => Self::Lowlatency,
            ffi::OSTREE_STATIC_DELTA_GENERATE_OPT_MAJOR => Self::Major,
            value => Self::__Unknown(value),
}
}
}

