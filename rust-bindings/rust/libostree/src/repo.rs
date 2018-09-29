use auto::Repo;

use gio;
use glib;
use glib::IsA;

pub trait RepoExtManual {
    fn new_for_str(path: &str) -> Repo;
}

impl<O: IsA<Repo> + IsA<glib::Object> + Clone + 'static> RepoExtManual for O {
    fn new_for_str(path: &str) -> Repo {
        Repo::new(&gio::File::new_for_path(path))
    }
}
