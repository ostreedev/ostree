extern crate proc_macro;

use proc_macro::TokenStream;
use proc_macro2::Span;
use quote::quote;

/// Wraps function using `procspawn` to allocate a new temporary directory,
/// make it the process' working directory, and run the function.
#[proc_macro_attribute]
pub fn itest(attrs: TokenStream, input: TokenStream) -> TokenStream {
    let attrs = syn::parse_macro_input!(attrs as syn::AttributeArgs);
    if attrs.len() > 1 {
        return syn::Error::new_spanned(&attrs[1], "itest takes 0 or 1 attributes")
            .to_compile_error()
            .into();
    }
    let destructive = match attrs.get(0) {
        Some(syn::NestedMeta::Meta(syn::Meta::NameValue(namevalue))) => {
            if let Some(name) = namevalue.path.get_ident().map(|i| i.to_string()) {
                if name == "destructive" {
                    match &namevalue.lit {
                        syn::Lit::Bool(v) => v.value,
                        _ => {
                            return syn::Error::new_spanned(
                                &attrs[1],
                                format!("destructive must be bool {}", name),
                            )
                            .to_compile_error()
                            .into();
                        }
                    }
                } else {
                    return syn::Error::new_spanned(
                        &attrs[1],
                        format!("Unknown argument {}", name),
                    )
                    .to_compile_error()
                    .into();
                }
            } else {
                false
            }
        }
        Some(v) => {
            return syn::Error::new_spanned(&v, "Unexpected argument")
                .to_compile_error()
                .into()
        }
        None => false,
    };
    let func = syn::parse_macro_input!(input as syn::ItemFn);
    let fident = func.sig.ident.clone();
    let varident = quote::format_ident!("ITEST_{}", fident);
    let fidentstrbuf = format!(r#"{}"#, fident);
    let fidentstr = syn::LitStr::new(&fidentstrbuf, Span::call_site());
    let testident = if destructive {
        quote::format_ident!("{}", "DESTRUCTIVE_TESTS")
    } else {
        quote::format_ident!("{}", "NONDESTRUCTIVE_TESTS")
    };
    let output = quote! {
        #[linkme::distributed_slice(#testident)]
        #[allow(non_upper_case_globals)]
        static #varident : Test = Test {
            name: #fidentstr,
            f: #fident,
        };
        #func
    };
    output.into()
}
