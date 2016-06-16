# Managing content in OSTree repositories

Once you have a build system going, if you actually want client
systems to retrieve the content, you will quickly feel a need for
"repository management".

The command line tool `ostree` does cover some core functionality, but
doesn't include very high level workflows.  One reason is that how
content is delivered and managed has concerns very specific to the
organization.  For example, some operating system content vendors may
want integration with a specific errata notification system when
generating commits.

In this section, we will describe some high level ideas and methods
for managing content in OSTree repositories, mostly independent of any
particular model or tool.  That said, a goal is to include at least
some sample scripts and workflows upstream in a potential new
"contrib" git repository.

One example of software which can assist in managing OSTree
repositories today is the [Pulp Project](http://www.pulpproject.org/),
which has a
[Pulp OSTree plugin](https://pulp-ostree.readthedocs.org/en/latest/).

## Mirroring repositories

It's very common to want to perform a full or partial mirror, in
particular across organizational boundaries (e.g. an upstream OS
provider, and a user that wants offline and faster access to the
content).  OSTree supports both full and partial mirroring of the base
`archive-z2` content, although not yet of static deltas.

To create a mirror, first create an `archive-z2` repository (you don't
need to run this as root), then add the upstream as a remote, then use
`pull --mirror`.

```
ostree --repo=repo init --mode=archive-z2
ostree --repo=repo remote add exampleos https://exampleos.com/ostree/repo
ostree --repo=repo pull --mirror exampleos:exampleos/x86_64/standard
```

You can use the `--depth=-1` option to retrieve all history, or a
positive integer like `3` to retrieve just the last 3 commits.

## Separate development vs release repositories

By default, OSTree accumulates server side history.  This is actually
optional in that your build system can (using the API) write a commit
with no parent.  But first, we'll investigate the ramifications of
server side history.

Many content vendors will want to separate their internal development
with what is made public to the world.  Therefore, you will want (at
least) two OSTree repositories, we'll call them "dev" and "prod".

To phrase this another way, let's say you have a continuous delivery
system which is building from git and committing into your "dev"
OSTree repository.  This might happen tens to hundreds of times per
day.  That's a substantial amount of history over time, and it's
unlikely most of your content consumers (i.e. not developers/testers)
will be interested in all of it.

The original vision of OSTree was to fulfill this "dev" role, and in
particular the "archive-z2" format was designed for it.

Then, what you'll want to do is promote content from "dev" to "prod".
We'll discuss this later, but first, let's talk about promotion
*inside* our "dev" repository.

## Promoting content along OSTree branches - "buildmaster", "smoketested"

Besides multiple repositories, OSTree also supports multiple branches
inside one repository, equivalent to git's branches.  We saw in an
earlier section an example branch name like
`exampleos/x86_64/standard`.  Choosing the branch name for your "prod"
repository is absolutely critical as client systems will reference it.
It becomes an important part of your face to the world, in the same
way the "master" branch in a git repository is.

But with your "dev" repository internally, it can be very useful to
use OSTree's branching concepts to represent different stages in a
software delivery pipeline.

Deriving from `exampleos/x86_64/standard`, let's say our "dev"
repository contains `exampleos/x86_64/buildmaster/standard`.  We choose the
term "buildmaster" to represent something that came straight from git
master.  It may not be tested very much.

Our next step should be to hook up a testing system (Jenkins,
Buildbot, etc.) to this.  When a build (commit) passes some tests, we
want to "promote" that commit.  Let's create a new branch called
`smoketested` to say that some basic sanity checks pass on the
complete system.  This might be where human testers get involved, for
example.

The build system can "promote" the `buildmaster` commit that passed
testing like this:

```
ostree commit -b exampleos/x86_64/smoketested/standard -s 'Passed tests' --tree=ref=aec070645fe53...
```

Here we're generating a new commit object (perhaps include in the commit
log links to build logs, etc.), but we're reusing the *content* from the `buildmaster`
commit `aec070645fe53` that passed the smoketests.

We can easily generalize this model to have an arbitrary number of
stages like `exampleos/x86_64/stage-1-pass/standard`,
`exampleos/x86_64/stage-2-pass/standard`, etc. depending on business
requirements and logic.

In this suggested model, the "stages" are increasingly expensive.  The
logic is that we don't want to spend substantial time on e.g. network
performance tests if something basic like a systemd unit file fails on
bootup.


## Promoting content between OSTree repositories

Now, we have our internal continuous delivery stream flowing, it's
being tested and works.  We want to periodically take the latest
commit on `exampleos/x86_64/stage-3-pass/standard` and expose it in
our "prod" repository as `exampleos/x86_64/standard`, with a much
smaller history.

We'll have other business requirements such as writing release notes
(and potentially putting them in the OSTree commit message), etc.

In [Build Systems](buildsystem-and-repos.md) we saw how the
`pull-local` command can be used to migrate content from the "build"
repository (in `bare-user` mode) into an `archive-z2` repository for
serving to client systems.

Following this section, we now have three repositories, let's call
them `repo-build`, `repo-dev`, and `repo-prod`.  We've been pulling
content from `repo-build` into `repo-dev` (which involves gzip
compression among other things since it is a format change).

When using `pull-local` to migrate content between two `archive-z2`
repositories, the binary content is taken unmodified.  Let's go ahead
and generate a new commit in our prod repository:

```
checksum=$(ostree --repo=repo-dev rev-parse exampleos/x86_64/stage-3-pass/standard`)
ostree --repo=repo-prod pull-local repo-dev ${checksum}
ostree --repo=repo-prod commit -b exampleos/x86_64/standard \
       -s 'Release 1.2.3' --add-metadata-string=ostree.version=1.2.3 \
	   --tree=ref=${checksum}
```

There are a few things going on here.  First, we found the latest
commit checksum for the "stage-3 dev", and told `pull-local` to copy
it, without using the branch name.  We do this because we don't want
to expose the `exampleos/x86_64/stage-3-pass/standard` branch name in
our "prod" repository.

Next, we generate a new commit in prod that's referencing the exact
binary content in dev.  If the "dev" and "prod" repositories are on
the same Unix filesystem, (like git) OSTree will make use of hard
links to avoid copying any content at all - making the process very
fast.

Another interesting thing to notice here is that we're adding an
`ostree.version` metadata string to the commit.  This is an optional
piece of metadata, but we are encouraging its use in the OSTree
ecosystem of tools.  Commands like `ostree admin status` show it by
default.

## Derived data - static deltas and the summary file

As discussed in [Formats](formats.md), the `archive-z2` repository we
use for "prod" requires one HTTP fetch per client request by default.
If we're only performing a release e.g. once a week, it's appropriate
to use "static deltas" to speed up client updates.

So once we've used the above command to pull content from `repo-dev`
into `repo-prod`, let's generate a delta against the previous commit:

```
ostree --repo=repo-prod static-delta generate exampleos/x86_64/standard
```

We may also want to support client systems upgrading from *two*
commits previous.

```
ostree --repo=repo-prod static-delta generate --from=exampleos/x86_64/standard^^ --to=exampleos/x86_64/standard
```

Generating a full permutation of deltas across all prior versions can
get expensive, and there is some support in the OSTree core for static
deltas which "recurse" to a parent.  This can help create a model
where clients download a chain of deltas.  Support for this is not
fully implemented yet however.

Regardless of whether or not you choose to generate static deltas,
you should update the summary file:

```
ostree --repo=repo-prod summary -u
```

(Remember, the `summary` command cannot be run concurrently, so this
 should be triggered serially by other jobs).

There is some more information on the design of the summary file in
[Repo](repo.md).

## Pruning our build and dev repositories

First, the OSTree author believes you should *not* use OSTree as a
"primary content store".  The binaries in an OSTree repository should
be derived from a git repository.  Your build system should record
proper metadata such as the configuration options used to generate the
build, and you should be able to rebuild it if necessary.  Art assets
should be stored in a system that's designed for that
(e.g. [Git LFS](https://git-lfs.github.com/)).

Another way to say this is that five years down the line, we are
unlikely to care about retaining the exact binaries from an OS build
on Wednesday afternoon three years ago.

We want to save space and prune our "dev" repository.

```
ostree --repo=repo-dev prune --refs-only --keep-younger-than="6 months ago"
```

That will truncate the history older than 6 months.  Deleted commits
will have "tombstone markers" added so that you know they were
explicitly deleted, but all content in them (that is not referenced by
a still retained commit) will be garbage collected.
