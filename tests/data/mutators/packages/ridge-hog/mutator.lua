function mutate(ctx)
  return {
    units = function(units, ctx)
      local kept = {}
      while true do
        kept[#kept + 1] = {}
      end
    end
  }
end
