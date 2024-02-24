---
nav_order: 190
---

# Contributing
{: .no_toc }

1. TOC
{:toc}

<!-- SPDX-License-Identifier: (CC-BY-SA-3.0 OR GFDL-1.3-or-later) -->

## Submitting patches

A majority of current maintainers prefer the GitHub pull request
model, and this motivated moving the primary git repository to
<https://github.com/ostreedev/ostree>.

However, we do not use the "Merge pull request" button, because we do
not like merge commits for one-patch pull requests, among other
reasons.  See [this issue](https://github.com/isaacs/github/issues/2)
for more information.  Instead, we use an instance of
[Homu](https://github.com/servo/homu), currently known as
`cgwalters-bot`.

As a review proceeds, the preferred method is to push `fixup!` commits. Any commits committed with the `--fixup` option will have have the word `fixup!` in its commit title. This is to indicate that this particular commit will be squashed with the commit that was specified in this command, `git commit --fixup <commit ref or hash>`. Homu knows how to use `--autosquash` when performing the final merge.

See the
[Git documentation](https://git-scm.com/docs/git-rebase) for more
information.

Alternative methods if you don't like GitHub (also fully supported):

 1. Send mail to <ostree-list@gnome.org>, with the patch attached
 1. Attach them to <https://bugzilla.gnome.org/>

It is likely however once a patch is ready to apply a maintainer
will push it to a GitHub PR, and merge via Homu.

## Commit message style

Please look at `git log` and match the commit log style, which is very
similar to the
[Linux kernel](https://git.kernel.org/cgit/linux/kernel/git/torvalds/linux.git).

You may use `Signed-off-by`, but we're not requiring it.

**General Commit Message Guidelines**:

1. Title
    - Specify the context or category of the changes e.g. `lib` for library changes, `docs` for document changes, `bin/<command-name>` for command changes, etc.
    - Begin the title with the first letter of the first word capitalized.
    - Aim for less than 50 characters, otherwise 72 characters max.
    - Do not end the title with a period.
    - Use an [imperative tone](https://en.wikipedia.org/wiki/Imperative_mood).
2. Body
    - Separate the body with a blank line after the title.
    - Begin a paragraph with the first letter of the first word capitalized.
    - Each paragraph should be formatted within 72 characters.
    - Content should be about what was changed and why this change was made.
    - If your commit fixes an issue, the commit message should end with `Closes: #<number>`.

Commit Message example:

```bash
<context>: Less than 50 characters for subject title

A paragraph of the body should be within 72 characters.

This paragraph is also less than 72 characters.
```

For more information see [How to Write a Git Commit Message](https://chris.beams.io/posts/git-commit/)

**Editing a Committed Message:**

To edit the message from the most recent commit run `git commit --amend`. To change older commits on the branch use `git rebase -i`. For a successful rebase have the branch track `upstream main`. Once the changes have been made and saved, run `git push --force origin <branch-name>`.

## Running the test suite

OSTree uses both `make check` and supports the
[Installed Tests](https://wiki.gnome.org/GnomeGoals/InstalledTests)
model as well (if `--enable-installed-tests` is provided).

## Coding style

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

## Contributing Tutorial

For a detailed walk-through on building, modifying, and testing, see this [tutorial on how to start contributing to OSTree](contributing-tutorial.md).

## Release process

Releases can be performed by [creating a new release ticket][new-release-ticket] and following the steps in the checklist there.

[new-release-ticket]: https://github.com/ostreedev/ostree/issues/new?labels=kind/release&template=release-checklist.md
