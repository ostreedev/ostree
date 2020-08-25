use gio::NONE_CANCELLABLE;
use glib::{Bytes, Variant};
use ostree::{prelude::*, Sign};

#[test]
fn sign_api_should_work() {
    let dummy_sign = Sign::get_by_name("dummy").unwrap();
    assert_eq!(dummy_sign.get_name().unwrap(), "dummy");

    let result = dummy_sign.data(&Bytes::from_static(b"1234"), NONE_CANCELLABLE);
    assert!(result.is_err());

    let result = dummy_sign.data_verify(&Bytes::from_static(b"1234"), &Variant::from("1234"));
    assert!(result.is_err());

    let result = Sign::get_by_name("NOPE");
    assert!(result.is_err());
}
