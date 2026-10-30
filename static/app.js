(function () {
  document.addEventListener("click", function (event) {
    var button = event.target.closest("[data-copy]");
    if (!button) return;
    var text = button.getAttribute("data-copy");
    var done = function () {
      var label = button.textContent;
      button.textContent = "Copied";
      setTimeout(function () {
        button.textContent = label;
      }, 1500);
    };
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(done);
      return;
    }
    var field = button.parentElement.querySelector("input");
    if (field) {
      field.select();
      document.execCommand("copy");
      done();
    }
  });

  document.querySelectorAll("select[data-ref]").forEach(function (select) {
    select.addEventListener("change", function () {
      window.location.href = select.getAttribute("data-base") + "?ref=" + encodeURIComponent(select.value);
    });
  });

  var pollers = document.querySelectorAll("[data-poll]");
  if (pollers.length) {
    var seen = {};
    setInterval(function () {
      pollers.forEach(function (node) {
        var url = node.getAttribute("data-poll");
        if (seen[url]) return;
        seen[url] = true;
        fetch(url, { cache: "no-store" })
          .then(function (response) {
            return response.json();
          })
          .then(function (data) {
            if (data.status !== "importing") {
              window.location.reload();
            }
          })
          .catch(function () {})
          .finally(function () {
            seen[url] = false;
          });
      });
    }, 2000);
  }
})();
