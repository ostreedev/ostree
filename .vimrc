set expandtab
set tabstop=8
set softtabstop=2
set shiftwidth=2

" Let's try to not go longer than 92
set textwidth=92
autocmd BufNewFile,BufRead *.c,*.h set textwidth=92

" This is the same as the default minus 0{, which is really annoying otherwise
" with our coding style.
set cinkeys=0},0),:,0#,!^F,o,O,e
