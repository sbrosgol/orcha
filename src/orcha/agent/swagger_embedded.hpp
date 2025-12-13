#pragma once

namespace Orcha::Agent {

// Embedded Swagger UI HTML served at /swagger
inline constexpr const char kSwaggerHtml[] = R"HTML(<!DOCTYPE html>
<html>
  <head>
    <meta charset="utf-8" />
    <title>Orcha API Docs</title>
    <link rel="stylesheet" href="https://unpkg.com/swagger-ui-dist@5/swagger-ui.css" />
  </head>
  <body>
    <div id="swagger-ui"></div>
    <script src="https://unpkg.com/swagger-ui-dist@5/swagger-ui-bundle.js"></script>
    <script>
      window.onload = () => {
        SwaggerUIBundle({
          url: '/swagger.json',
          dom_id: '#swagger-ui',
          presets: [SwaggerUIBundle.presets.apis],
        });
      };
    </script>
  </body>
</html>
)HTML";

} // namespace Orcha::Agent
