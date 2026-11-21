---
name: http-endpoint
description: Add or change an Orcha HTTP endpoint (agent/routes/*) - IRouteHandler, registration in CommandAgent, auth, bounded body parsing (JSON/YAML), status codes and error shapes, run-resource replies, OpenAPI in SwaggerRoute, README, tests and smoke scenarios. Use when adding a route, changing a response shape or status code, or exposing a new admin API for the dashboard.
---

# HTTP endpoints

Changing a public request/response shape, status code, auth requirement or
default is a user decision (CLAUDE.md "How we work"): propose it, then build.
Additive fields in responses are fine; removing or renaming is not.

## Anatomy

- A route is an `IRouteHandler` (`agent/IRouteHandler.hpp`): `can_handle`,
  `handle` (synchronous, runs on the server's worker pool) and `get_routes`
  (the route's self-description). Put it in `agent/routes/<Name>Route.hpp`, construct it
  with what it needs (no globals, no service locator inside), and register it in
  `CommandAgent` (`agent/CommandAgent.cpp`), gated on the feature's config like
  the plugin-upload and jobs routes.
- **Never block a worker unboundedly.** Long work goes through `RunQueue`
  (runs) or returns 202; waits are capped (`server.max_wait_ms`).
- Layering: the agent depends on core/workflow/jobs interfaces, not the reverse,
  and builds without yaml-cpp (use `agent/HttpBody.hpp`, not `YamlJson.cpp`
  internals).

## Auth (`agent/AuthMiddleware.hpp`)

- `/api/*` is admin Basic auth automatically; anything an operator manages
  belongs under `/api/`.
- `POST /workflow`-style execution endpoints need the bearer token
  (`server.api_token`) or admin Basic, and fail closed (503) on a non-loopback
  bind without a token. Reuse that check; don't invent a new scheme.
- A new unauthenticated route needs an explicit reason (health, the static
  `/admin` page). Never put secrets or credentials in query strings.

## Input

- JSON bodies: `parse_json_bounded()` (depth 64). JSON-or-YAML bodies:
  `parse_request_body()` (`agent/HttpBody.hpp`), which dispatches on
  `Content-Type` and applies the YAML limits. Never call `Json::parse` on a body.
- Validate shape and types explicitly; definitions go through
  `WorkflowDefinition::from_json` / `JobDefinition::from_json`; catch their
  exceptions and reply 400 with the message (pattern: `WorkflowRoute::submit`,
  `JobRoute::parse_job`), and body parse errors via `reply_bad_body`.
- Path parameters and IDs: validate format and length before using them in
  file paths, SQL (always bound parameters) or logs.

## Output

- JSON via the `agent/HttpJson.hpp` helpers (`reply_json`, `reply_error`); serialise with the UTF-8
  `replace` handler when it can contain step output.
- Errors: `{"error": "<message>"}` with the right status — 400 invalid input,
  401/403 auth, 404 unknown resource, 409 state conflict (include what the
  client must do, e.g. `requires: allow_downgrade`), 413 too large, 429 +
  `Retry-After` for a full queue, 503 only for "disabled"/shutting down.
- Anything that starts a run answers with the run resource through
  `agent/routes/RunResponses.hpp` (202 + `Location`, `Prefer: wait`); there is
  one run shape.

## Docs

- OpenAPI: add the path in `SwaggerRoute::build_paths` (`agent/routes/SwaggerRoute.hpp`)
  with request/response schemas and every status code.
- README section for the feature; if the dashboard uses it, the API contract
  list in the `admin-ui` skill and the field list in the `admin-ui-reviewer`
  agent.

## Tests

- Route-level tests calling `handle()` with a built `HttpRequest` (see
  `tests/JobStoreTests.hpp`, `tests/P0RegressionTests.hpp`): happy path, each
  error status, oversized/deep/malformed bodies, wrong types.
- End to end on the real binary: add a scenario to
  `.claude/skills/server-smoke/scripts/smoke.sh` (auth, status codes, headers)
  or `scripts/admin_smoke.sh` for `/api/*`; run the `server-smoke` skill.
- Review: `architecture-reviewer` for new public surface or auth changes.
