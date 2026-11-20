# plan.md — the work that is still open
this is a living task list.
when work is complete, edit this document, add any additional user guidance received during the session, and move the work item to Awaiting User Verification, when completed.
do not edit or remove empty headers, or document format examples.
working rules, standards, and the commit and push discipline live in `dev.md`; machine access specifics live in `C:\Projects\local.md`.
always read this file immediately before making updates: the user may change it at any time. never revert user changes: write around them.

## 1. TODO - not actionable yet.
these tasks are upcoming, but some of the details have not been firmly decided.
do not edit, or act on anything in this section, unless instructed by user

## 2. PENDING - actionable work.
user may add or update items here during your session- read it carefully before making updates.
keep report on changes BRIEF. The entire section should not exceed 20 lines.

review format. do not edit or remove this format:

- <job title>
  Prompt: <full user message>
  Follow-on guidance: <summary of any changes the user makes during the session>
  Changes: <all files modified, summary with line change counts, brief functional summaries, etc>

- Functional audit of every file and function, fix all bugs, clean up code
  Prompt: "perform a thorough functional audit on the entire project. fix all bugs. setup a goal, outlining your overall task. update plan.md with all bugs and intended changes, before and after making changes. carefully read plan.md. remove unneeded comments. clean up dead code/files. clean up backwards wording. clean up un readable code. perform a complete end to end audit of EVERY file, every function. this is a long running task. do not worry about context limits. do not attempt to mark anything complete, without actually checking - be thorough!"
  Follow-on guidance: "additionally: review all vendored code, and its source: see if it can be updated to a more recent version." / "only use run.bat, in a background job, to compile and run the server. do not attempt to come up with new deployment machinery." / "clean up plan.md. remove completed verified steps" / "clean up plan.md - validate all claims of completion; and remove finished work/logs. ensure only instructions, and pending/todo items remain."
  Changes: open findings are section 4; completed lines are removed once verified through the running server. nothing here is marked fixed until it has been compiled on X1 by run.bat and re-checked through the running server.

## 3. Awaiting User Verification
when work is complete, edit this document, add any additional user guidance received during the session, and move the work item here, when completed.
