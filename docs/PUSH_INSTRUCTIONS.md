# Push instructions

The local repository is built and committed. Publishing it is the owner's step, because it requires
authentication and a choice of visibility. The builder did not create the remote, did not run gh,
did not authenticate, and did not push. The local commit uses a neutral author, so the steps below
re-stamp it as the owner.

## Steps

1. Create an empty repository named CARMIX on your own account. Do not add a README, a license, or
   a .gitignore through the web interface, so the first push is clean. Push it private first, review
   it, then change it to public when you are ready.

2. Set the commit author to yourself and re-stamp the existing commit. Run these from the repository
   directory.

```
git config user.name "<your name or handle>"
git config user.email "<your email>"
git commit --amend --reset-author --no-edit
```

3. Add the remote and push. HTTPS variant:

```
git remote add origin https://github.com/<your-account>/CARMIX.git
git push -u origin main
```

SSH variant:

```
git remote add origin git@github.com:<your-account>/CARMIX.git
git push -u origin main
```

Creating the remote, choosing public or private, authenticating, and running the push are your
steps. The license is set to AGPL-3.0 under your copyright in the LICENSE file.
