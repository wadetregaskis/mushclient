--[[
  Spell checker for MUSHclient, written by Nick Gammon.
  Written:  9th October 2006
  Updated: 11th October 2006
  Updated:  6th March 2007 to make progress bar optional
  Updated: 13th April 2007 to added IGNORE_MIXED_CASE, IGNORE_IMBEDDED_NUMBERS
  Updated: 15th February 2009 to convert to using SQLite database instead of Lua table
  Updated: 21st February 2009 to fix problem where words with 2 metaphones were only stored once.
--]]

local SHOW_PROGRESS_BAR = true -- show progress bar? true or false

local METAPHONE_LENGTH = 4   -- how many characters of metaphone to get back
local EDIT_DISTANCE = 4      -- how close a word must be to appear in the list of suggestions
local CASE_SENSITIVE = false -- compare case? true or false
local IGNORE_CAPITALIZED = false -- ignore words starting with a capital? true or false
local IGNORE_MIXED_CASE = false  -- ignore words in MixedCase (like that one)? true or false
local IGNORE_IMBEDDED_NUMBERS = false -- ignore words with numbers in them? true or false

-- this is the pattern we use to find "words" in the text to be spell-checked
local pattern = "%a+'?[%a%d]+" -- regexp to give us a word with a possible single imbedded quote

-- Off Windows text is UTF-8, whose letters beyond ASCII are several bytes
-- that %a does not match: an accented "cafe" would be checked as "caf".  So every byte
-- of a multi-byte character counts as part of a word there.  (Windows speaks
-- the ANSI code page, whose letters %a already knows, and whose high bytes
-- include punctuation, like curly quotes, that must not.)
if package.config:sub (1, 1) ~= "\\" then
  pattern = "[%a\128-\255]+'?[%a%d\128-\255]+"
end -- not Windows

-- Dictionaries are read-only and bundled next to the executable (app_directory).
-- The user dictionary and the compiled spell.sqlite get WRITTEN, so they live in
-- a writable data directory (App Support on macOS/Linux; the app dir on Windows).
-- read-only dictionaries ship in the bundle's resource dir (Contents/Resources/
-- spell on macOS; next to the executable elsewhere)
local dict_directory = (utils.info ().resource_directory or utils.info ().app_directory) .. "spell/"
local directory      = utils.info ().data_directory or dict_directory
-- file name of the user dictionary, in the writable directory
local userdict = "userdict.txt"

-- The operating system's spell checker, if there is one: Windows', macOS's, or
-- Enchant's (Hunspell's) on Linux -- in whatever language the user has set,
-- with whatever words they have taught it.  utils.spell_engine names it, or
-- returns nil.  With one, the dictionaries below are not used at all: a word
-- is right if the system says so or the user added it to MUSHclient's own
-- dictionary (userdict.txt), which keeps the words a MUD uses out of the
-- dictionary every other program shares -- or, if they choose "Add to
-- System" instead, puts them in it.
local system = utils.spell_engine and utils.spell_engine ()

-- stuff below used internally
local make_upper  -- this becomes the upper-case conversion function, see below
local db          -- SQLite3 dictionary database (without a system spell checker)
local user_words = {}  -- MUSHclient's own dictionary: userdict.txt, upper-cased
local cancelmessage = "spell check cancelled"
local previousword  --> not used right now
local change, ignore  -- tables of change-all, ignore-all words

-- dictionaries - add new entries along similar lines to add more dictionary files
local files = {

-- lower-case words

  "english-words.10",
  "english-words.20",
  "english-words.35",
  "english-words.40",

-- upper case words

  "english-upper.10",
  "english-upper.35",
  "english-upper.40",

-- American words

  "american-words.10",
  "american-words.20",

-- contractions (eg. aren't, doesn't)

  "english-contractions.10",
  "english-contractions.35",
  
-- user dictionary
  userdict,
  }
  
 -- trim leading and trailing spaces from a string
local function trim (s)
  return (string.gsub (s, "^%s*(.-)%s*$", "%1"))
end -- trim

-- insert a word into our metaphone table - called by reading dictionaries
-- and also by adding a word during the spellcheck
local function insert_word (word, user)

  word = trim (word)   -- strip CR/LF (dict files are CRLF) + surrounding space
  if word == "" then
    return
  end -- empty word
  
              
  -- get both metaphones
  local m1, m2 = utils.metaphone (word, METAPHONE_LENGTH)
  local fixed_word = string.gsub (word, "'", "''")  -- convert ' to ''
  
  assert (db:execute (string.format ("INSERT INTO words VALUES (NULL, '%s', '%s', %i)",
            fixed_word, m1, user)));
    
  -- do 2nd metaphone, if any
   if m2 then
    assert (db:execute (string.format ("INSERT INTO words VALUES (NULL, '%s', '%s', %i)",
              fixed_word, m2, user)));
   end -- having alternative

end -- insert_word

-- is it in MUSHclient's own dictionary?
local function user_word (word)
  return user_words [make_upper (word)] ~= nil
end -- user_word

-- add a word to MUSHclient's own dictionary: userdict.txt, which the database
-- is built from -- so the word survives a rebuild, or a change of spell checker
local function add_user_word (word)
  word = trim (word)
  if word == "" or user_word (word) then
    return
  end -- nothing to add
  user_words [make_upper (word)] = true
  -- not run on to the end of a last line with no newline (Notepad's way)
  local newline = ""
  local f = io.open (directory .. userdict, "rb")
  if f then
    if f:seek ("end") > 0 then
      f:seek ("end", -1)
      local last = f:read (1)
      if last ~= "\n" and last ~= "\r" then
        newline = "\n"
      end -- no newline at the end
    end -- not empty
    f:close ()
  end -- opened
  f = io.open (directory .. userdict, "a")
  if f then
    f:write (newline, word, "\n")
    f:close ()
  end -- opened
  if db then
    insert_word (word, 1)
  end -- have database
end -- add_user_word


-- sort function for sorting the suggestions into edit-distance order
local function suggestions_compare (word)
   return function (a, b)
     local diff = utils.edit_distance (make_upper (a), word) - 
                  utils.edit_distance (make_upper (b), word) 
     if diff == 0 then
       return make_upper (a) < make_upper (b)
     else
       return diff < 0
     end -- differences the same?
  end -- compareit
end -- function suggestions_compare

-- check for one word, called by spellcheck (invokes suggestion dialog)
local function checkword_and_suggest (word)

  if IGNORE_CAPITALIZED then
    -- ignore words starting in caps
    if string.find (word, "^[A-Z]") then
      return word, "ignore"
    end -- this round, ignore this word
  end -- if IGNORE_CAPITALIZED

  if IGNORE_MIXED_CASE  then
    -- ignore words in mixed case
    if string.find (word, "[A-Z]") and
       string.find (word, "[a-z]") then
      return word, "ignore"
    end -- this round, ignore this word
  end -- if IGNORE_MIXED_CASE

  if IGNORE_IMBEDDED_NUMBERS  then
    -- ignore words with numbers in them
    if string.find (word, "%d") then
      return word, "ignore"
    end -- this round, ignore this word
  end -- if IGNORE_IMBEDDED_NUMBERS

  uc_word = make_upper (word)  -- convert to upper-case if wanted

  -- if we already did "ignore all" on this particular word, ignore it again
  if ignore [word] then
    return word, "ignore"
  end -- this round, ignore this word
  
  -- if we said change A to B, change it again
  if change [word] then
    return change [word], "change"
  end -- change to this word

  local suggestions

  if system then
    if user_word (word) or utils.spell_check (word) then
      return word, "ok"
    end -- word found
    suggestions = utils.spell_suggest (word) or {}
  else

  -- MUSHclient's own dictionary first: a word can be in userdict.txt and not
  -- yet in the database, which reads it only when first built
  if user_word (word) then
    return word, "ok"
  end -- one of theirs

  -- table of suggestions, based on the metaphone
  local keyed_suggestions = {}
  
  -- get both metaphones
  local m1, m2 = utils.metaphone (word, METAPHONE_LENGTH)
  
  local function lookup_metaphone (m)
    local found = false
    for row in db:rows(string.format ("SELECT name FROM words WHERE metaphone = '%s'", m)) do
      local word = trim (row [1])   -- existing DBs were built from CRLF dicts
      if make_upper (word) == uc_word then
        found = true -- found exact match
        break
      end  -- found
      if utils.edit_distance (make_upper (word), uc_word) < EDIT_DISTANCE then
        keyed_suggestions [word] = true
      end -- close enough
    end
    return found
  end -- lookup_metaphone
  
  -- look up first metaphone
  if lookup_metaphone (m1) then
    return word, "ok"
  end -- word found

  -- try 2nd metaphone
  if m2 then
    if lookup_metaphone (m2) then
      return word, "ok"
    end -- word found
  end -- have alternate metaphone

  -- pull into indexed table
  suggestions = {}
  for k in pairs (keyed_suggestions) do
    table.insert (suggestions, k)
  end -- for

  table.sort (suggestions, suggestions_compare (uc_word))

  end -- no system spell checker

  -- not found? do spell check dialog (offering "Add to System" if there is one)
  local action, replacement = utils.spellcheckdialog (word, suggestions, system ~= nil)
  
  -- they cancelled?
  if not action then
    error (cancelmessage)  --> forces us out of gsub loop
  end -- cancelled
  
  -- ignore this only - just return
  if action == "ignore" then
    return word, "ignore" -- use current word
  end -- ignore word

  -- ignore all of this word? add to list
  if action == "ignoreall" then
    ignore [word] = true
    return word, "ignore" -- use current word
  end -- ignore word

  -- add to user dictionary? 
  -- add to metaphone table, and rewrite dictionary
  if action == "add" then
    add_user_word (word)
    return word, "ok"
  end -- adding

  -- add to the system's dictionary, shared with every other program
  if action == "addsystem" then
    if not utils.spell_add_word (word) then
      -- kept somewhere, rather than looking added and asked about next time
      add_user_word (word)
      if utils.msgbox then
        utils.msgbox ("The system dictionary would not take \"" .. word ..
                      "\", so it has been added to MUSHclient's own dictionary instead.",
                      "Spell check", "ok", "!")
      end -- can say so
    end -- system refused
    return word, "ok"
  end -- adding to the system
  
  -- change word once? return replacement
  if action == "change" then
     return checkword_and_suggest (replacement)  -- however, re-check it
  end -- changing
  
  -- change all occurrences? add to table, return replacement
  if action == "changeall" then
    local newword, newaction = checkword_and_suggest (replacement) -- re-check it
    if newaction == "ok" then
       change [word] = newword
    end -- if approved
    return newword  -- return the new word
  end -- changing
  
  error "unexpected result from dialog"
end -- checkword_and_suggest

-- exported function to do the spellcheck
function spellcheck (line)
  change = {}  -- words to change
  ignore = {}  -- words to ignore
  
  -- we raise an error if they cancel the spell check dialog
  ok, result = xpcall (function () 
                        return string.gsub (line, pattern, checkword_and_suggest) 
                      end, debug.traceback)
 
  if ok then
    return result
  end -- not cancelled spell check

  -- whoops! syntax error?
  if not string.find (result, cancelmessage, 1, true) then
    error (result)
  end -- some syntax error
  
  return nil  --> shows they cancelled
end -- spellchecker

local notfound  -- table of not-found words, for spellcheck_string

-- check for one word, called by spellcheck_string
local function checkword (word)
  uc_word = make_upper (word)  -- convert to upper-case if wanted

  if system then
    if not (user_word (word) or utils.spell_check (word)) then
      table.insert (notfound, word)
    end -- not found
    return
  end -- system spell checker

  if user_word (word) then
    return
  end -- one of theirs

  -- get first metaphone
  local m = utils.metaphone (word, METAPHONE_LENGTH)
  
  local found = false
  for row in db:rows(string.format ("SELECT name FROM words WHERE metaphone = '%s'", m)) do
    local word = trim (row [1])   -- existing DBs were built from CRLF dicts
    if make_upper (word) == uc_word then
      found = true -- found exact match
      break
    end  -- found
  end
  
  if found then return end  -- do nothing if word found

  -- otherwise insert our word   
  table.insert (notfound, word) 
end -- function checkword

-- exported function to spellcheck a string
function spellcheck_string (text)
  notfound = {}
  string.gsub (text, pattern, checkword)
  return notfound
end -- spellcheck_string

-- exported function to add a word to the user dictionary
function spellcheck_add_word (word, action, replacement)
  assert (action == "i", "Can only use action 'i' in user dictionary")  -- only "i" supported right now
  add_user_word (word)
end -- spellcheck_string

-- read one of the dictionaries
local function read_dict (dlg, name)
 -- bundled dictionaries are read-only (dict_directory); the user dictionary is
 -- writable and lives in the data directory.
 local dir = (name == userdict) and directory or dict_directory
 if SHOW_PROGRESS_BAR then
   dlg:step ()
   dlg:status (dir .. name)
   if dlg:checkcancel () then
     error "Dictionary loading cancelled"
   end -- if cancelled
 end -- if SHOW_PROGRESS_BAR

  for line in io.lines (dir .. name) do
    insert_word (line, name == userdict and 1 or 0)
  end
end -- read_dict
  
local function init ()

  -- make a suitable function depending on whether they want case-sensitive or not
  if CASE_SENSITIVE then
    make_upper = function (s) return s end -- return original
  else
    make_upper = function (s) return s:upper () end  -- make upper case
  end -- case-sensitivity test
  
  -- if no user dictionary, create it
  local f = io.open (directory .. userdict, "r")
  if not f then
    f = io.output (directory .. userdict)
    f:close ()
  else
    f:close ()
  end -- checking for user dictionary

  for line in io.lines (directory .. userdict) do
    line = trim (line)
    if line ~= "" then
      user_words [make_upper (line)] = true
    end -- a word
  end -- reading user dictionary

  if system then
    -- No database: the system checks the words.  But a word added before
    -- this version, when Add put it only in the database, is carried over
    -- into userdict.txt (one already there is left as it is) -- once: the
    -- database is then put aside, so a word they later delete from
    -- userdict.txt stays deleted, and should the built-in lists be needed
    -- again they are rebuilt from userdict.txt.  A database that cannot be
    -- read is left alone; it is not needed here.
    local old = io.open (directory .. "spell.sqlite", "rb")
    if old then
      old:close ()
      local migrated = pcall (function ()
        local olddb = assert (sqlite3.open (directory .. "spell.sqlite"))
        for row in olddb:rows ("SELECT DISTINCT name FROM words WHERE user = 1") do
          add_user_word (row [1])
        end -- each added word
        olddb:close ()
      end)
      if migrated then
        os.remove (directory .. "spell-before-6.sqlite")
        os.rename (directory .. "spell.sqlite", directory .. "spell-before-6.sqlite")
      end -- carried over
    end -- have one
    return
  end -- system spell checker

  -- open database on disk
  db = assert (sqlite3.open( directory .. "spell.sqlite"))
  local words_table = false
  local count = 0
  
  -- if database just created, there won't be a words table
  for row in db:nrows("SELECT * FROM sqlite_master WHERE type = 'table' AND name = 'words'") do 
    if string.match (row.sql, "word_id") then  -- better be newer version
      words_table = true
    end -- if
  end
  
  -- enable WAL (Write-Ahead Logging)
  assert (db:execute "PRAGMA journal_mode=WAL;")
  
  -- if no words table, make one
  if not words_table then
      -- create a table to hold the words
    assert (db:execute[[
      DROP TABLE IF EXISTS words;
      CREATE TABLE words(
        word_id INTEGER NOT NULL PRIMARY KEY autoincrement,
        name  VARCHAR(10) NOT NULL,
        metaphone VARCHAR(10) NOT NULL,
        user INT(1)
      );
      CREATE INDEX metaphone_index ON words (metaphone);
      CREATE INDEX name_index ON words (name);
    ]])
  end -- if 
    
  -- check if table empty
  for row in db:rows('SELECT COUNT(*) FROM words') do 
    count = row [1] 
  end

  -- if empty, populate it  
  if count == 0 then
    
    local dlg
  
    if SHOW_PROGRESS_BAR then
      dlg = progress.new ("Loading dictionaries into SQLite database ...")
    
      dlg:range (0, #files)
      dlg:setstep (1)
    end -- if SHOW_PROGRESS_BAR
     
    assert (db:execute "BEGIN TRANSACTION");
    
    for k, v in ipairs (files) do
      ok, result = pcall (function () 
                            read_dict (dlg, v) 
                          end)
      if not ok then 
        if SHOW_PROGRESS_BAR then
          dlg:close ()
        end -- if SHOW_PROGRESS_BAR
        error (result)
      end -- not ok
    end -- reading each file
    
    if SHOW_PROGRESS_BAR then
      dlg:close ()
    end -- if SHOW_PROGRESS_BAR
    
    assert (db:execute "COMMIT");
    
  end -- if nothing in database
  
  
end -- init

-- when script is loaded, do initialization stuff

init ()

