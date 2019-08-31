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

#[test]
fn should_append_argv() {
    let mut args = KernelArgs::new();
    args.append_argv(&["arg1", "arg2=value", "arg3", "arg4=value"]);
    assert_eq!(args.to_string(), "arg1 arg2=value arg3 arg4=value");
}

#[test]
fn should_append_argv_filtered() {
    let mut args = KernelArgs::new();
    args.append_argv_filtered(
        &["prefix.arg1", "arg2", "prefix.arg3", "arg4=value"],
        &["prefix"],
    );
    assert_eq!(args.to_string(), "arg2 arg4=value");
}

#[test]
fn should_replace_argv() {
    let mut args = KernelArgs::from_string("arg1=value1 arg2=value2 arg3");
    args.replace_argv(&["arg1=value3", "arg3=value4", "arg4"]);
    assert_eq!(args.to_string(), "arg1=value3 arg2=value2 arg3=value4 arg4");
}
