use glib::GString;

use crate::Deployment;

impl Deployment {
    /// Access the name of the deployment stateroot.
    pub fn stateroot(&self) -> GString {
        self.osname()
    }
}
