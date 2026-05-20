# Agent Notes

- Keep documentation updated whenever code or architecture changes.
- Update `README.md` in the same change set when behavior/build/test steps are affected.
- Whenever `docker-compose.yml` is updated, update `docker-compose.submission.yml` in the same change set.

## Commit Message Pattern

- Use Conventional Commits in English: `<type>(<scope>): <subject>`.
- Keep the subject in imperative mood, lowercase, and without a trailing period.
- Prefer concise scopes that map to folders/components (for example: `server`, `load-balancer`, `scripts`, `docker`, `docs`).
- Add a body with short bullet points when the change is not trivial.
- The description/body can be a list, with one bullet per line and no blank lines between bullet points.

### Allowed Types

- `feat`: new behavior or capability
- `fix`: bug fix
- `perf`: performance improvement
- `refactor`: code change without behavior change
- `docs`: documentation-only changes
- `test`: tests added/updated
- `build`: build system/dependencies/tooling
- `ci`: CI/CD pipeline changes
- `chore`: maintenance tasks that do not fit types above
- `revert`: revert a previous commit

### Examples

- `perf(server): optimize transaction context parser`
- `fix(load-balancer): handle closed unix socket without spin loop`
- `docs(readme): document local benchmark workflow`
- Example with list body (no blank lines between bullets):
```text
perf(server): optimize transaction context parser and key dispatch
- replace string-heavy parsing with single-pass key dispatch
- reduce ctype usage with inline ASCII checks
- keep response behavior unchanged
```
