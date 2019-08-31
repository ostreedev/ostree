#[cfg(feature = "v2019_3")]
use crate::KernelArgs;

#[test]
fn should_create_and_fill_kernel_args() {
    let mut args = KernelArgs::new();
    args.append("key=value");
    args.append("arg1");
    args.append("key2=value2");
    assert_eq!(args.to_string(), "key=value arg1 key2=value2");
}

#[test]
fn should_convert_to_string_vec() {
    let mut args = KernelArgs::new();
    args.parse_append("key=value arg1 key2=value2");
    assert_eq!(
        args.to_strv()
            .iter()
            .map(|s| s.as_str())
            .collect::<Vec<_>>(),
        vec!["key=value", "arg1", "key2=value2"]
    );
}

#[test]
fn should_get_last_value() {
    let mut args = KernelArgs::new();
    args.append("key=value1");
    args.append("key=value2");
    args.append("key=value3");
    assert_eq!(args.get_last_value("key").unwrap(), "value3");
}

#[test]
fn should_convert_from_string() {
    let args = KernelArgs::from(String::from("arg1 arg2 arg3=value"));
    assert_eq!(args.to_strv(), vec!["arg1", "arg2", "arg3=value"]);
}
