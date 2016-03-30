Submitting patches
------------------

A majority of current maintainers prefer the Github pull request
model, and this motivated moving the primary git repository to
<https://github.com/ostreedev/ostree>.

However, we do not use the "Merge pull request" button, because we do
not like merge commits for one-patch pull requests, among other
reasons.  See [this issue](https://github.com/isaacs/github/issues/2)
for more information.  Instead, we use an instance of
[Homu](https://github.com/servo/homu), currently known as
`cgwalters-bot`.

As a review proceeeds, the preferred method is to push `fixup!`
commits via `git commit --fixup`.  Homu knows how to use
`--autosquash` when performing the final merge.  See the
[Git documentation](https://git-scm.com/docs/git-rebase]) for more
information.

Alternative methods if you don't like Github (also fully supported):

 1. Send mail to <ostree-list@gnome.org>, with the patch attached
 1. Attach them to <https://bugzilla.gnome.org/>

It is likely however once a patch is ready to apply a maintainer
will push it to a github PR, and merge via Homu.

Commit message style
--------------------

Please look at `git log` and match the commit log style, which is very
similar to the
[Linux kernel](https://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git).

You may use `Signed-off-by`, but we're not requiring it.

Running the test suite
----------------------

OSTree uses both `make check` and supports the
[Installed Tests](https://wiki.gnome.org/GnomeGoals/InstalledTests)
model as well (if `--enable-installed-tests` is provided).

Coding style
------------

Indentation is GNU.  Files should start with the appropriate mode lines.

Use GCC `__attribute__((cleanup))` wherever possible.  If interacting
with a third party library, try defining local cleanup macros.

Use GError and GCancellable where appropriate.

Prefer returning `gboolean` to signal success/failure, and have output
values as parameters.

Prefer linear control flow inside functions (aside from standard
loops).  In other words, avoid "early exits" or use of `goto` besides
`goto out;`.

This is an example of an "early exit":

    static gboolean
    myfunc (...)
    {
        gboolean ret = FALSE;

        /* some code */

        /* some more code */

        if (condition)
          return FALSE;

        /* some more code */

        ret = TRUE;
      out:
        return ret;
    }

If you must shortcut, use:

    if (condition)
      {
        ret = TRUE;
        goto out;
      }

A consequence of this restriction is that you are encouraged to avoid
deep nesting of loops or conditionals.  Create internal static helper
functions, particularly inside loops.  For example, rather than:

    while (condition)
      {
        /* some code */
        if (condition)
          {
             for (i = 0; i < somevalue; i++)
               {
                  if (condition)
                    {
                      /* deeply nested code */
                    }

                    /* more nested code */
               }
          }
      }

Instead do this:

    static gboolean
    helperfunc (..., GError **error)
    {
      if (condition)
       {
         /* deeply nested code */
       }

      /* more nested code */

      return ret;
    }

    while (condition)
      {
        /* some code */
        if (!condition)
          continue;

        for (i = 0; i < somevalue; i++)
          {
            if (!helperfunc (..., i, error))
              goto out;
          }
      }
