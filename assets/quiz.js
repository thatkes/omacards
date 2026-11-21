/* Reusable retrieval-practice widget.
 *
 * Usage in a lesson:
 *   <div class="quiz" data-quiz="quiz-1"></div>
 *   <script>
 *     window.LESSON_QUIZZES = window.LESSON_QUIZZES || {};
 *     window.LESSON_QUIZZES["quiz-1"] = [
 *       { question: "...", options: ["...", "...", "...", "..."],
 *         answer: 2, why: "..." },
 *       ...
 *     ];
 *   </script>
 *   <script src="../assets/quiz.js"></script>
 *
 * Design rules enforced by convention: options are equal-length strings,
 * correct-answer position varies between questions, and no formatting clue
 * distinguishes the answer before submission.
 */
(function () {
  function render(host, questions) {
    let index = 0;
    let correct = 0;

    const counter = document.createElement("div");
    counter.className = "quiz-counter";
    host.appendChild(counter);

    const questionEl = document.createElement("div");
    questionEl.className = "quiz-question";
    host.appendChild(questionEl);

    const optionsEl = document.createElement("div");
    optionsEl.className = "quiz-options";
    host.appendChild(optionsEl);

    const feedbackEl = document.createElement("div");
    feedbackEl.className = "quiz-feedback";
    host.appendChild(feedbackEl);

    function updateCounter() {
      counter.textContent = `Question ${Math.min(index + 1, questions.length)} of ${questions.length} · ${correct} correct`;
    }

    function showQuestion() {
      const q = questions[index];
      questionEl.textContent = q.question;
      optionsEl.replaceChildren();
      feedbackEl.textContent = "";
      feedbackEl.className = "quiz-feedback";

      q.options.forEach(function (option, i) {
        const button = document.createElement("button");
        button.type = "button";
        button.className = "quiz-option";
        button.textContent = option;
        button.addEventListener("click", function () {
          const right = i === q.answer;
          if (right) correct += 1;
          Array.from(optionsEl.children).forEach(function (child, j) {
            child.disabled = true;
            if (j === q.answer) child.classList.add("correct");
            else if (j === i) child.classList.add("wrong");
          });
          feedbackEl.textContent = (right ? "Right. " : "Not quite. ") + q.why;
          feedbackEl.className = "quiz-feedback " + (right ? "good" : "bad");
          updateCounter();

          const next = document.createElement("button");
          next.type = "button";
          next.className = "quiz-option";
          next.textContent = index + 1 < questions.length ? "Next question" : "Finish quiz";
          next.addEventListener("click", function () {
            if (index + 1 < questions.length) {
              index += 1;
              next.remove();
              showQuestion();
            } else {
              questionEl.textContent = "Done.";
              optionsEl.replaceChildren();
              feedbackEl.textContent = `You got ${correct} of ${questions.length} correct.`;
              feedbackEl.className = "quiz-feedback";
              counter.textContent = "";
            }
          });
          optionsEl.appendChild(next);
        });
        optionsEl.appendChild(button);
      });
      updateCounter();
    }

    showQuestion();
  }

  document.addEventListener("DOMContentLoaded", function () {
    const quizzes = window.LESSON_QUIZZES || {};
    document.querySelectorAll("[data-quiz]").forEach(function (host) {
      const key = host.getAttribute("data-quiz");
      if (quizzes[key]) render(host, quizzes[key]);
    });
  });
})();
