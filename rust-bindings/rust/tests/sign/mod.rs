use ostree::prelude::*;
use ostree::{gio, glib};

#[test]
fn sign_api_should_work() {
    let dummy_sign = ostree::Sign::by_name("dummy").unwrap();
    assert_eq!(dummy_sign.name().unwrap(), "dummy");

    let result = ostree::prelude::SignExt::data(
        &dummy_sign,
        &glib::Bytes::from_static(b"1234"),
        gio::NONE_CANCELLABLE,
    );
    assert!(result.is_err());

    let result = dummy_sign.data_verify(&glib::Bytes::from_static(b"1234"), &"1234".to_variant());
    assert!(result.is_err());

    let result = ostree::Sign::by_name("NOPE");
    assert!(result.is_err());
}
